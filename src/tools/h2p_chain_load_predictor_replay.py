#!/usr/bin/env python3
"""Replay simple address predictors on H2P-chain target-load raw streams.

Input rows are produced by Scarab's h2p_chain_load_raw_stream_dump option.
Each dynamic access is predicted before the predictor observes that access,
then the predictor state is updated with the actual address.
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


LINE_BITS = 6


def parse_int(value: str) -> int:
    return int(value, 0)


def pct(num: int, den: int) -> str:
    return f"{(100.0 * num / den):.6f}" if den else "0.000000"


class Row:
    __slots__ = (
        "path",
        "seq",
        "off_path",
        "is_target",
        "pc",
        "vaddr",
        "line_index",
    )

    def __init__(self, path: Path, fields: Dict[str, str]) -> None:
        self.path = path
        self.seq = parse_int(fields["seq"])
        self.off_path = bool(parse_int(fields["off_path"]))
        self.is_target = bool(parse_int(fields["is_target_h2p_chain_load"]))
        self.pc = parse_int(fields["load_pc"])
        self.vaddr = parse_int(fields["vaddr"])
        self.line_index = parse_int(fields["line_index"])


class Prediction:
    __slots__ = ("vaddr", "line_index")

    def __init__(self, vaddr: Optional[int], line_index: Optional[int]) -> None:
        self.vaddr = vaddr
        self.line_index = line_index


class Predictor:
    name = "base"
    granularity = "unknown"

    def predict(self, row: Row) -> Optional[Prediction]:
        raise NotImplementedError

    def update(self, row: Row) -> None:
        raise NotImplementedError


class LastValuePredictor(Predictor):
    def __init__(self, name: str, field: str) -> None:
        self.name = name
        self.granularity = field
        self.field = field
        self.last: Dict[int, int] = {}

    def _value(self, row: Row) -> int:
        return row.vaddr if self.field == "vaddr" else row.line_index

    def predict(self, row: Row) -> Optional[Prediction]:
        if row.pc not in self.last:
            return None
        value = self.last[row.pc]
        if self.field == "vaddr":
            return Prediction(value, value >> LINE_BITS)
        return Prediction(None, value)

    def update(self, row: Row) -> None:
        self.last[row.pc] = self._value(row)


class StridePredictor(Predictor):
    def __init__(self, name: str, field: str, confidence: int) -> None:
        self.name = name
        self.granularity = field
        self.field = field
        self.confidence = confidence
        self.last: Dict[int, int] = {}
        self.stride: Dict[int, int] = {}
        self.conf: Dict[int, int] = defaultdict(int)

    def _value(self, row: Row) -> int:
        return row.vaddr if self.field == "vaddr" else row.line_index

    def predict(self, row: Row) -> Optional[Prediction]:
        if row.pc not in self.last or row.pc not in self.stride:
            return None
        if self.conf[row.pc] < self.confidence:
            return None
        value = self.last[row.pc] + self.stride[row.pc]
        if self.field == "vaddr":
            return Prediction(value, value >> LINE_BITS)
        return Prediction(None, value)

    def update(self, row: Row) -> None:
        value = self._value(row)
        if row.pc in self.last:
            delta = value - self.last[row.pc]
            if row.pc in self.stride and self.stride[row.pc] == delta:
                self.conf[row.pc] += 1
            else:
                self.stride[row.pc] = delta
                self.conf[row.pc] = 1
        self.last[row.pc] = value


class TopDeltaPredictor(Predictor):
    def __init__(self, name: str, field: str, min_count: int) -> None:
        self.name = name
        self.granularity = field
        self.field = field
        self.min_count = min_count
        self.last: Dict[int, int] = {}
        self.delta_counts: Dict[int, Counter[int]] = defaultdict(Counter)

    def _value(self, row: Row) -> int:
        return row.vaddr if self.field == "vaddr" else row.line_index

    def predict(self, row: Row) -> Optional[Prediction]:
        if row.pc not in self.last or not self.delta_counts[row.pc]:
            return None
        delta, count = self.delta_counts[row.pc].most_common(1)[0]
        if count < self.min_count:
            return None
        value = self.last[row.pc] + delta
        if self.field == "vaddr":
            return Prediction(value, value >> LINE_BITS)
        return Prediction(None, value)

    def update(self, row: Row) -> None:
        value = self._value(row)
        if row.pc in self.last:
            self.delta_counts[row.pc][value - self.last[row.pc]] += 1
        self.last[row.pc] = value


class MarkovPredictor(Predictor):
    def __init__(self, name: str, field: str, min_count: int) -> None:
        self.name = name
        self.granularity = field
        self.field = field
        self.min_count = min_count
        self.last: Dict[int, int] = {}
        self.transitions: Dict[Tuple[int, int], Counter[int]] = defaultdict(Counter)

    def _value(self, row: Row) -> int:
        return row.vaddr if self.field == "vaddr" else row.line_index

    def predict(self, row: Row) -> Optional[Prediction]:
        if row.pc not in self.last:
            return None
        counts = self.transitions.get((row.pc, self.last[row.pc]))
        if not counts:
            return None
        value, count = counts.most_common(1)[0]
        if count < self.min_count:
            return None
        if self.field == "vaddr":
            return Prediction(value, value >> LINE_BITS)
        return Prediction(None, value)

    def update(self, row: Row) -> None:
        value = self._value(row)
        if row.pc in self.last:
            self.transitions[(row.pc, self.last[row.pc])][value] += 1
        self.last[row.pc] = value


class Stats:
    __slots__ = (
        "train_rows",
        "target_accesses",
        "predictions",
        "correct_line",
        "correct_vaddr",
    )

    def __init__(self) -> None:
        self.train_rows = 0
        self.target_accesses = 0
        self.predictions = 0
        self.correct_line = 0
        self.correct_vaddr = 0


def discover_inputs(paths: Iterable[str]) -> List[Path]:
    files: List[Path] = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            files.extend(sorted(path.rglob("h2p_chain_load_raw_stream.out")))
        else:
            files.extend(sorted(path.parent.glob(path.name)))
    return sorted(dict.fromkeys(files))


def make_predictors(args: argparse.Namespace) -> List[Predictor]:
    available = {
        "last_vaddr":
            lambda: LastValuePredictor("last_vaddr", "vaddr"),
        "last_line":
            lambda: LastValuePredictor("last_line", "line"),
        "pc_stride_vaddr":
            lambda: StridePredictor("pc_stride_vaddr", "vaddr",
                                    args.stride_confidence),
        "pc_stride_line":
            lambda: StridePredictor("pc_stride_line", "line",
                                    args.stride_confidence),
        "pc_top_delta_vaddr":
            lambda: TopDeltaPredictor("pc_top_delta_vaddr", "vaddr",
                                      args.min_count),
        "pc_top_delta_line":
            lambda: TopDeltaPredictor("pc_top_delta_line", "line",
                                      args.min_count),
        "pc_markov_vaddr":
            lambda: MarkovPredictor("pc_markov_vaddr", "vaddr",
                                    args.min_count),
        "pc_markov_line":
            lambda: MarkovPredictor("pc_markov_line", "line",
                                    args.min_count),
    }
    selected = args.predictors or list(available.keys())
    unknown = [name for name in selected if name not in available]
    if unknown:
        raise SystemExit(f"Unknown predictor(s): {', '.join(unknown)}")
    return [available[name]() for name in selected]


def should_eval(row: Row, include_off_path: bool) -> bool:
    return row.is_target and (include_off_path or not row.off_path)


def should_train(row: Row, args: argparse.Namespace) -> bool:
    if row.off_path and not args.train_off_path:
        return False
    if args.train_all_rows:
        return True
    return row.is_target


def update_stats(stats: Stats, row: Row, pred: Optional[Prediction]) -> None:
    stats.target_accesses += 1
    if pred is None:
        return
    stats.predictions += 1
    if pred.line_index is not None and pred.line_index == row.line_index:
        stats.correct_line += 1
    if pred.vaddr is not None and pred.vaddr == row.vaddr:
        stats.correct_vaddr += 1


def run(
    args: argparse.Namespace,
) -> Tuple[Dict[str, Stats], Dict[Tuple[str, int], Stats], Dict[str, str]]:
    predictors = make_predictors(args)
    granularity = {predictor.name: predictor.granularity for predictor in predictors}
    stats = {predictor.name: Stats() for predictor in predictors}
    per_pc: Dict[Tuple[str, int], Stats] = defaultdict(Stats)
    rows_seen = 0
    start_time = time.monotonic()

    for path in discover_inputs(args.inputs):
        file_rows = 0
        file_start = time.monotonic()
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for fields in reader:
                row = Row(path, fields)
                rows_seen += 1
                file_rows += 1
                eval_row = should_eval(row, args.eval_off_path)
                train_row = should_train(row, args)
                for predictor in predictors:
                    pred = predictor.predict(row) if eval_row else None
                    if eval_row:
                        update_stats(stats[predictor.name], row, pred)
                        update_stats(per_pc[(predictor.name, row.pc)], row, pred)
                    if train_row:
                        predictor.update(row)
                        stats[predictor.name].train_rows += 1
                if args.progress_rows and rows_seen % args.progress_rows == 0:
                    elapsed = time.monotonic() - start_time
                    rate = rows_seen / elapsed if elapsed else 0.0
                    print(
                        f"[replay] rows={rows_seen} "
                        f"elapsed={elapsed:.1f}s rate={rate:.0f} rows/s "
                        f"file={path}",
                        file=sys.stderr,
                        flush=True,
                    )
        if args.progress_files:
            elapsed = time.monotonic() - file_start
            rate = file_rows / elapsed if elapsed else 0.0
            print(
                f"[replay] finished file rows={file_rows} "
                f"elapsed={elapsed:.1f}s rate={rate:.0f} rows/s file={path}",
                file=sys.stderr,
                flush=True,
            )

    return stats, per_pc, granularity


def vaddr_metric_fields(item: Stats, granularity: str) -> List[object]:
    if granularity != "vaddr":
        return ["NA", "NA", "NA"]
    return [
        item.correct_vaddr,
        pct(item.correct_vaddr, item.predictions),
        pct(item.correct_vaddr, item.target_accesses),
    ]


def write_summary(path: Path, stats: Dict[str, Stats], granularity: Dict[str, str]) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "predictor",
            "prediction_granularity",
            "train_rows",
            "target_accesses",
            "predictions",
            "coverage_pct",
            "correct_line",
            "line_accuracy_pct",
            "line_recall_pct",
            "correct_vaddr",
            "vaddr_accuracy_pct",
            "vaddr_recall_pct",
        ])
        for name, item in sorted(stats.items()):
            writer.writerow([
                name,
                granularity[name],
                item.train_rows,
                item.target_accesses,
                item.predictions,
                pct(item.predictions, item.target_accesses),
                item.correct_line,
                pct(item.correct_line, item.predictions),
                pct(item.correct_line, item.target_accesses),
                *vaddr_metric_fields(item, granularity[name]),
            ])


def write_per_pc(
    path: Path,
    per_pc: Dict[Tuple[str, int], Stats],
    granularity: Dict[str, str],
) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "predictor",
            "prediction_granularity",
            "load_pc",
            "target_accesses",
            "predictions",
            "coverage_pct",
            "correct_line",
            "line_accuracy_pct",
            "line_recall_pct",
            "correct_vaddr",
            "vaddr_accuracy_pct",
            "vaddr_recall_pct",
        ])
        for (name, pc), item in sorted(per_pc.items()):
            writer.writerow([
                name,
                granularity[name],
                f"0x{pc:x}",
                item.target_accesses,
                item.predictions,
                pct(item.predictions, item.target_accesses),
                item.correct_line,
                pct(item.correct_line, item.predictions),
                pct(item.correct_line, item.target_accesses),
                *vaddr_metric_fields(item, granularity[name]),
            ])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+",
                        help="raw stream files, globs, or directories to scan")
    parser.add_argument("--output", required=True,
                        help="predictor-level summary CSV path")
    parser.add_argument("--per-pc-output",
                        help="optional per-load-PC summary CSV path")
    parser.add_argument("--stride-confidence", type=int, default=2,
                        help="matching deltas required before stride prediction")
    parser.add_argument("--min-count", type=int, default=2,
                        help="minimum observed count for top-delta/Markov prediction")
    parser.add_argument("--predictors", nargs="+",
                        help="predictors to run; default runs all predictors")
    parser.add_argument("--progress-rows", type=int, default=0,
                        help="print progress every N input rows to stderr")
    parser.add_argument("--progress-files", action="store_true",
                        help="print a progress line after each raw stream file")
    parser.add_argument("--train-off-path", action="store_true",
                        help="include off-path rows in predictor training")
    parser.add_argument("--eval-off-path", action="store_true",
                        help="include off-path target rows in evaluation")
    parser.add_argument("--train-all-rows", action="store_true",
                        help="train on all rows instead of only target rows")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    stats, per_pc, granularity = run(args)
    write_summary(Path(args.output), stats, granularity)
    if args.per_pc_output:
        write_per_pc(Path(args.per_pc_output), per_pc, granularity)


if __name__ == "__main__":
    main()

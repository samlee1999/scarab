# ZERECO — H2P-Chain Load Acceleration for Early Branch Misprediction Resolution

> Research summary & experiment log. Last updated 2026-07-12 (Scarab `test` branch).
> This document consolidates the core idea, related-work positioning, mechanism design,
> and all experimental results to date. It supersedes finishing TEA end-to-end; TEA is now
> a comparison baseline.

---

## 1. Goal & Core Insight

**Goal**: Detect branch mispredictions as early as possible to cut the misprediction penalty,
especially for **H2P (Hard-to-Predict) branches**.

**Key insight**: A mispredicted H2P branch resolves late mainly because a **load inside its
dependence chain** is slow. TEA (MICRO 2024) attacks this with a separate precomputation
thread — fast and accurate but very high complexity. Instead, if we simply **reduce the
latency of the loads inside H2P dependence chains**, the H2P branch resolves early at execute,
with no separate thread.

**Motivating oracle**: forcing every H2P-chain load to 1-cycle latency gives (geomean Periodic IPC):
- **+32%** vs baseline OoO with the data prefetcher OFF (run `260625_perf_comparison`).
- **+28.4%** vs a modern baseline with the golden_cove stream prefetcher ON (run `260709_decoupling_RFP_L1P`).

That headroom is ~2× TEA's reported gain in the same simulator, at a fraction of the complexity.

---

## 2. Mechanism Design

Target **only loads that belong to an H2P dependence chain** (identified via the TEA
substrate: HBT → Fill Buffer → Backward Dataflow Walk → Dependency Chain Cache). Keeping the
target set small is what keeps the hardware light.

For each target load:
1. **Address predictable → prefetch.**
   - Exact address predicted → **register-file (RF) prefetch** (ISCA'22 RFP style): value delivered
     to the physical register; no-flush recovery via existing scheduler replay on a wrong address.
   - Only the cache line predicted → **L1 prefetch**: brings the line into L1 (correct but weaker).
2. **Unpredictable → PUBS-style issue priority** (MICRO'18): schedule the chain earlier in the IQ.
   Correct but saves only a few arbitration cycles; treat as a floor, not a co-equal pillar.
3. A **table-based per-PC confidence classifier** decides predictable vs not (stride / top-delta).

**Complexity story (staged)**: ① H2P-targeted L1 prefetch (no correctness machinery) →
② RFP-style RF delivery (reuse no-flush replay) → ③ PUBS priority fallback (IQ untouched).

---

## 3. Related-Work Positioning

| Work | What it does | Gap we fill |
|------|--------------|-------------|
| **TEA** (MICRO'24) | Precompute H2P chains in a separate thread; late result → early flush | Never characterizes chain-load latency/predictability; no prefetcher baseline; dismisses backend prioritization (CRISP) in one sentence. High complexity (Block Cache 19KB, Fill Buffer 8KB, 192 RS/PR reserved, +31.9% dynamic insts). |
| **DLVP** (MICRO'17) | Path-based address prediction → early cache probe → value | Direct evidence: perlbmk +71% *because* value-predicted loads enable **early misprediction resolution**. But no criticality targeting. |
| **RFP** (ISCA'22) | Stride-based prefetch into the physical register file at rename; no-flush recovery | **Explicitly defers criticality-based targeting to future work** — exactly our H2P-chain filter. Hides only L1 latency (rename-launch). |
| **PUBS** (MICRO'18) | Prioritize unconfident-branch-slice ops in the IQ | Removes only issue-arbitration cycles; disables itself at high LLC MPKI. Bounds our "unpredictable" fallback. |
| **Prefetch survey** (2020) | Strided/spatial/temporal taxonomy | Only temporal captures pointer chasing, at MB-scale metadata. Filtering to H2P-chain loads makes small on-chip temporal/Markov tables affordable. |

Must-check before novelty claims: **CRISP** (ASPLOS'22, compiler criticality slices), **Focused
Value Prediction** (ISCA'20), **Hermes** (MICRO'22, off-chip load prediction), **Branch
Runahead** (MICRO'21), **SLB** (HPCA'13).

Our unique coordinate: *use H2P-chain membership as a criticality filter so a cheap address
predictor + RF prefetch can attack the misprediction penalty directly.*

---

## 4. Experimental Infrastructure (Scarab)

- **Oracle** (`h2p_chain_perfect_load`): all on-path H2P-chain loads forced to a fixed latency
  (`dcache_stage.c:dcache_stage_try_main_chain_load_oracle`). Store-forwarded loads excluded.
- **Access-pattern profiling** (`h2p_chain_load_pattern_profile`): per-PC reuse/stride/delta stats.
- **Raw-stream dump** (`h2p_chain_load_raw_stream_dump`) + **offline replay**
  (`src/tools/h2p_chain_load_predictor_replay.py`): last-value / stride / top-delta / Markov,
  at vaddr and cache-line granularity, on/off-path filters.
- **Online predictor oracle** (this project): the oracle is gated on an *online* per-PC predictor
  that mirrors the offline replay algorithms, so measured IPC = the recoverable fraction of the
  oracle upper bound. Params in `core.param.def`:
  `h2p_chain_oracle_predictor` (0=none/all, 1=stride, 2=top-delta),
  `h2p_chain_oracle_granularity` (0=vaddr/RF, 1=line/L1),
  `h2p_chain_oracle_hit_latency` (vaddr latency, default 1),
  `h2p_chain_oracle_stride_confidence` (2), `h2p_chain_oracle_min_count` (2).
  Line-granularity applies a real L1-hit latency (`DCACHE_CYCLES + extra_ld_latency`), not a flat
  constant, so `vaddr − line` isolates the RF-vs-L1 benefit. A **first-visit guard**
  (`op->dcache_cycle == MAX_CTR`) trains each dynamic load exactly once (program order), keeping
  sim coverage aligned with the offline replay.

**Predictor algorithms**
- *stride*: one delta per PC; predicts `last + stride` only after the same delta repeats
  `confidence` (=2) times. Narrow but accurate; good for regular strided access.
- *top-delta*: per-PC histogram of deltas (16 slots); predicts `last + most_frequent_delta`
  (count ≥ `min_count`=2). Broad coverage, lower accuracy — cheap when recovery is no-flush.

---

## 5. Results

### 5.1 Access-pattern characterization (`260624`, top-5-weight simpoints, weighted avg)
- H2P-chain target loads = **52%** of on-path loads.
- Dcache hit **83%**, memory access **12%**, **store-forwarding ~0.08%** (→ no LSCD blacklist needed).
- Avg latency 23.7 cyc, but bimodal by workload: L1-hit-dominated leela 5.5 / sssp 7.3 vs pr 117.7.
- Per-PC top-4 delta predictable: **byte 82% / line 86%** (bfs/cc/pr 94-98%; omnetpp/mcf/leela ~61-68%).
- Address repeatability **bimodal**: 61% of accesses repeat ≥128×, 22% repeat <8× (little in between).
- Top-5 PCs cover 43% of accesses (GAP high: pr 91%, sssp 80%; SPEC/DC lower).

**→ Refutes the "H2P loads are inherently unpredictable" risk: they are largely predictable.**

### 5.2 Offline predictor replay (180M accesses)
| predictor | coverage | accuracy | recall |
|-----------|----------|----------|--------|
| pc_stride (vaddr) | 70.0% | 94.7% | 66.3% |
| pc_top_delta (vaddr) | 99.9% | 81.5% | — |
| last_value (vaddr) | 99.98% | 38.1% | — |

`last_value` low despite 61% repeatability → repeated addresses are interleaved (Markov/temporal territory).

### 5.3 Performance & the RF-vs-L1 decomposition (`260709`, prefetcher-ON, geomean Periodic IPC)
Baseline = golden_cove with the default stream prefetcher ON. clang/1305 excluded (flaky
"no forward progress" ASSERT in 3 predictor configs — a known, ignored Scarab watchdog).

| config | vs baseline | % of oracle headroom |
|--------|-------------|----------------------|
| **oracle** (all chain loads latency=1) | **+28.41%** | 100% |
| **pred_top_delta_vaddr** (RF) | **+12.06%** | 42.5% |
| pred_stride_vaddr (RF) | +10.01% | 35.2% |
| pred_top_delta_line (L1) | +4.54% | 16.0% |
| pred_stride_line (L1) | +1.16% | 4.1% |

**RF vs L1 (vaddr − line):** stride RF-extra **+8.85pp**, top-delta **+7.52pp**.

**→ ~80-90% of the benefit needs RF-level exact-vaddr delivery; L1-prefetch alone recovers almost
nothing.** Reason: ~83% of chain loads already hit L1, so bringing the line to L1 is redundant; the
real cost is the accumulated L1-hit-use latency across the dependent chain, removable only by
delivering the value to the register. **Design conclusion: the mechanism must be register-file
prefetch, not L1 prefetch.**

The stream prefetcher shrank the oracle headroom from +32% (OFF) to +28.4% (ON): it already absorbs
the easy strided chain loads; the remaining +28% is what a general prefetcher cannot capture — the
motivation for H2P-chain-targeted RF prefetch.

### 5.4 Implementation cross-validation
Online predictor sim coverage matches the offline replay almost exactly:
- stride vaddr: **online 65.8% cover @ 94.7% acc** vs offline 66.3% @ 94.7%.
- top-delta vaddr: online 75.8% cover @ 76.0% acc (16-slot histogram approximation).

top-delta beats stride because its make-rate is 99.8% vs 69.4% and no-flush recovery makes its
lower 76% accuracy costless → **pred_top_delta_vaddr is the best realistic config**.

Per-benchmark: biggest realistic wins on high-predictability workloads (xgboost, mcf, bfs, cc, tc);
realistic ≪ oracle on low-predictability / long-latency ones (xz oracle +57% vs realistic +7%,
omnetpp, pr) — the temporal/Markov gap.

---

## 6. Key Insights (one-liners)

1. Each link of the causal chain is already validated in prior work; only the *combination*
   (H2P-criticality-filtered address prediction → RF prefetch → early branch resolution) is new.
2. Oracle headroom is ~2× TEA at far lower complexity, and survives a strong prefetcher baseline.
3. H2P-chain loads are largely predictable (delta 82-86%), refuting the inverse-correlation risk.
4. The benefit is fundamentally **RF-level** (exact address → register), not L1-level — because the
   loads mostly already hit L1 and the bottleneck is hit-use latency accumulated along the chain.
5. Under no-flush recovery, a broad low-accuracy predictor (top-delta) beats a narrow high-accuracy
   one (stride).

---

## 7. Open Questions / Next Steps

- Close the low-predictability gap (xz, omnetpp, pr): add a Markov/temporal predictor; measure
  whether it recovers the interleaved-repeat addresses that stride/delta miss.
- Model prefetch **timeliness** explicitly (predict N-ahead with in-flight count, RFP-style) — the
  online predictor is the substrate that can do this; offline replay cannot.
- Quantify the "unpredictable" PUBS-style fallback contribution separately.
- Wrong-prediction cost: current experiments assume no-flush (wrong → not idealized); add the
  scheduler-replay penalty for a realistic net number.
- Confirm novelty vs CRISP / Focused VP / Hermes / Branch Runahead / SLB.

---

## 8. File / Run Index
- Code: `src/dcache_stage.c` (oracle + online predictor), `src/core.param.def` (params),
  `src/tea/tea.stat.def` (`H2P_CHAIN_LOAD_ORACLE_PRED_*`).
- Offline tool: `src/tools/h2p_chain_load_predictor_replay.py`.
- Descriptor: `~/scarab-infra/json/zereco_dbg.json` (configs baseline / oracle /
  pred_{stride,top_delta}_{vaddr,line}, all prefetcher-ON).
- Runs: `~/simulations/260625_perf_comparison` (prefetcher-OFF oracle),
  `~/simulations/260624_h2p_chain_load_access_pattern_all_simpoints` (access pattern + offline replay),
  `~/simulations/260709_decoupling_RFP_L1P` (RF-vs-L1 decomposition; graphs + collected_stats.csv here).
- Reference papers: `/home/lee/scarab/reference/` (TEA, PUBS, RFP, DLVP, prefetch survey).

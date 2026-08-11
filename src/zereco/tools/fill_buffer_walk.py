#!/usr/bin/env python3
"""Backward dataflow walk over Scarab fill_buffer.out dumps.

Mirrors ZERECO/TEA identification: start at an H2P branch, walk youngest->oldest,
track live register + memory producers, mark slice members.
"""
import re, sys, glob, os
from collections import defaultdict, Counter

ENTRY = re.compile(
    r'^\[\s*(\d+)\]\s+PC:\s+(0x[0-9a-f]+)\s+\|\s+OpNum:\s+(\d+)\s+\|\s+H2P:\s+(\S)\s+\|\s+Disasm:\s+(.*?)\s*$')
MEMREF = re.compile(r'(\d+)@([0-9a-f]+)\s*$')
REG = re.compile(r'r(\d+)\(([^)]+)\)')


class Op:
    __slots__ = ('idx', 'pc', 'opnum', 'h2p', 'opcode', 'dsts', 'srcs',
                 'memsize', 'memaddr', 'raw')

    def __init__(self, idx, pc, opnum, h2p, disasm):
        self.idx, self.pc, self.opnum, self.h2p = idx, pc, opnum, h2p
        self.raw = disasm
        parts = disasm.split(None, 1)
        self.opcode = parts[0]
        rest = parts[1] if len(parts) > 1 else ''
        self.memsize = self.memaddr = None
        m = MEMREF.search(rest)
        if m:
            self.memsize = int(m.group(1))
            self.memaddr = int(m.group(2), 16)
            rest = rest[:m.start()].strip()
        if '<-' in rest:
            dpart, spart = rest.split('<-', 1)
        else:
            dpart, spart = '', rest          # no dst: all listed regs are sources
        self.dsts = [g[0] for g in REG.findall(dpart)]
        self.srcs = [g[0] for g in REG.findall(spart)]

    def is_load(self):
        return self.opcode == 'ILD' and self.memaddr is not None

    def is_store(self):
        return self.opcode == 'IST' and self.memaddr is not None

    def short(self):
        d = ','.join(regname(r) for r in self.dsts)
        s = ','.join(regname(r) for r in self.srcs)
        t = f'{self.opcode:<8}'
        if d:
            t += f'{d} <- {s}'
        else:
            t += f'{s}'
        if self.memaddr is not None:
            t += f'  [{self.memsize}@0x{self.memaddr:x}]'
        return t


REGNAMES = {}


def regname(r):
    return REGNAMES.get(r, f'r{r}')


def parse(path):
    ops = []
    for line in open(path, errors='replace'):
        m = ENTRY.match(line.rstrip('\n'))
        if not m:
            continue
        idx, pc, opnum, h2p, dis = m.groups()
        for num, nm in REG.findall(dis):
            REGNAMES[num] = nm
        ops.append(Op(int(idx), pc, int(opnum), h2p == 'O', dis))
    return ops


def walk(ops, start, max_ops=64, max_scan=400):
    """Backward dataflow walk from ops[start] (an H2P branch)."""
    br = ops[start]
    live_reg = set(br.srcs)
    live_mem = set()
    slice_idx = [start]
    scanned = 0
    for i in range(start - 1, -1, -1):
        scanned += 1
        if scanned > max_scan or len(slice_idx) > max_ops:
            break
        op = ops[i]
        hit = False
        if any(d in live_reg for d in op.dsts):
            hit = True
        # store -> load memory producer
        if op.is_store() and op.memaddr in live_mem:
            hit = True
        if not hit:
            continue
        slice_idx.append(i)
        for d in op.dsts:
            live_reg.discard(d)
        if op.is_store():
            live_mem.discard(op.memaddr)
        live_reg.update(op.srcs)
        if op.is_load():
            live_mem.add(op.memaddr)      # look for the store that produced it
    return sorted(slice_idx)


def analyze(path, top=3):
    ops = parse(path)
    if not ops:
        return
    name = '/'.join(path.split('/')[-4:-1])
    h2p_idx = [o.idx for o in ops if o.h2p]
    by_pc = Counter(ops[i].pc for i in h2p_idx)
    print(f'\n{"="*78}\n{name}   ops={len(ops)}  H2P dynamic={len(h2p_idx)}  '
          f'H2P static PCs={len(by_pc)}')
    for pc, cnt in by_pc.most_common(top):
        # pick a late instance so the walk has history behind it
        insts = [i for i in h2p_idx if ops[i].pc == pc]
        pick = insts[len(insts) // 2] if len(insts) > 1 else insts[0]
        sl = walk(ops, pick)
        loads = [i for i in sl if ops[i].is_load()]
        span = pick - sl[0]
        print(f'\n--- H2P PC {pc}   instances={cnt}   slice={len(sl)} ops, '
              f'loads={len(loads)}, spans {span} retired ops')
        for i in sl:
            tag = 'H2P>' if ops[i].h2p else ('LD  ' if ops[i].is_load()
                                             else ('ST  ' if ops[i].is_store() else '    '))
            print(f'   {tag} [{i:4}] {ops[i].pc}  {ops[i].short()}')
        # address stream of each target load PC across the whole buffer
        for li in loads:
            lpc = ops[li].pc
            addrs = [o.memaddr for o in ops if o.pc == lpc and o.is_load()]
            if len(addrs) < 2:
                continue
            deltas = [addrs[k + 1] - addrs[k] for k in range(len(addrs) - 1)]
            dc = Counter(deltas)
            top_d, top_n = dc.most_common(1)[0]
            print(f'      load {lpc}: {len(addrs)} accesses, '
                  f'dominant delta={top_d} ({100*top_n//len(deltas)}% of {len(deltas)}), '
                  f'unique addrs={len(set(addrs))}')


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    for f in sorted(glob.glob(os.path.join(root, '**', 'fill_buffer.out'),
                              recursive=True)):
        analyze(f)

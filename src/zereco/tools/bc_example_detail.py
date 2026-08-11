#!/usr/bin/env python3
"""Deeper characterisation of the GAP BC motivating example."""
import re, sys
from collections import Counter, defaultdict

P = '/home/lee/simulations/zereco_260811_retired_stream_examples/oldest_first_retired_stream_log/gapbs/gapbs/bc_g19_n100/4496/fill_buffer.out'
ENTRY = re.compile(r'^\[\s*(\d+)\]\s+PC:\s+(0x[0-9a-f]+)\s+\|\s+OpNum:\s+(\d+)\s+\|\s+H2P:\s+(\S)\s+\|\s+Disasm:\s+(.*?)\s*$')
MEM = re.compile(r'(\d+)@([0-9a-f]+)\s*$')

rows = []
for line in open(P, errors='replace'):
    m = ENTRY.match(line.rstrip('\n'))
    if m:
        i, pc, opn, h2p, dis = m.groups()
        mm = MEM.search(dis)
        addr = int(mm.group(2), 16) if mm else None
        rows.append((int(i), pc, int(opn), h2p == 'O', dis.split()[0], addr))

LD1, LD2 = '0x7ffff3daa100', '0x7ffff3daa108'
H2P1, H2P2, H2P3 = '0x7ffff3daa10d', '0x7ffff3daa0bb', '0x7ffff3daa0fe'

print(f'total retired uops in window: {len(rows)}')

# ---- loop iteration length -------------------------------------------------
ld1_idx = [r[0] for r in rows if r[1] == LD1]
gaps = [ld1_idx[k+1] - ld1_idx[k] for k in range(len(ld1_idx)-1)]
gc = Counter(gaps)
print(f'\nLD1 instances: {len(ld1_idx)}')
print(f'uops per loop iteration (gap between LD1 instances): {gc.most_common(6)}')
print(f'  -> mean {sum(gaps)/len(gaps):.1f} uops/iteration')

# ---- how many dynamic instances of LD1 fit in a 512-entry ROB --------------
mean_gap = sum(gaps)/len(gaps)
print(f'  -> with ROB=512: ~{512/mean_gap:.1f} concurrent LD1 instances in flight')
print(f'  -> RFP in-flight counter must therefore predict base + 4*N for N up to ~{int(512/mean_gap)}')

# ---- LD1 address stream ----------------------------------------------------
a1 = [r[5] for r in rows if r[1] == LD1 and r[5] is not None]
d1 = [a1[k+1]-a1[k] for k in range(len(a1)-1)]
print(f'\nLD1 addr: {hex(min(a1))} .. {hex(max(a1))}  span={max(a1)-min(a1)}B')
print(f'  deltas: {Counter(d1).most_common(4)}   unique={len(set(a1))}/{len(a1)}')
print(f'  distinct 4KB pages touched: {len(set(a >> 12 for a in a1))}')
print(f'  distinct 64B lines touched: {len(set(a >> 6 for a in a1))}  -> {len(a1)/len(set(a>>6 for a in a1)):.1f} accesses/line')

# ---- LD2 address stream ----------------------------------------------------
a2 = [r[5] for r in rows if r[1] == LD2 and r[5] is not None]
d2 = [a2[k+1]-a2[k] for k in range(len(a2)-1)]
print(f'\nLD2 addr: {hex(min(a2))} .. {hex(max(a2))}  span={max(a2)-min(a2)}B ({(max(a2)-min(a2))/2**20:.1f} MB)')
print(f'  deltas: top4={Counter(d2).most_common(4)}   unique={len(set(a2))}/{len(a2)}')
print(f'  distinct 4KB pages touched: {len(set(a >> 12 for a in a2))}')
print(f'  distinct 64B lines touched: {len(set(a >> 6 for a in a2))}  -> {len(a2)/len(set(a>>6 for a in a2)):.2f} accesses/line')
print(f'  delta range: min={min(d2)} max={max(d2)}')

# ---- H2P instance counts and interleaving ---------------------------------
print('\nH2P branch dynamic instances in window:')
for pc, nm in ((H2P1,'d == -1      '), (H2P2,'d == depth   '), (H2P3,'loop bound   ')):
    n = sum(1 for r in rows if r[1] == pc)
    print(f'  {pc} {nm}: {n}')

# ---- does LD2 value feed both branches without a reload? ------------------
# check that between H2P1 and H2P2 there is no second load of depths[v]
seq = [(r[0], r[1]) for r in rows]
pairs = 0
reload_between = 0
for k, (i, pc) in enumerate(seq):
    if pc != H2P1:
        continue
    for j in range(k+1, min(k+6, len(seq))):
        if seq[j][1] == H2P2:
            pairs += 1
            if any(seq[t][1] == LD2 for t in range(k+1, j)):
                reload_between += 1
            break
print(f'\nH2P1 -> H2P2 adjacency: {pairs} pairs, reload of depths[v] between them: {reload_between}')
print('  -> the SAME load result feeds both H2P branches (shared producer)')

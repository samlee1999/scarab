#!/usr/bin/env python3
"""Architecture diagrams for the six core logics of the critical-path slice design.

Logic 2 (LPR) and 3 (chain membership) are the user's own figure; this script draws
1 (H2P identification), 4 (Target Load + address predictability), 5 (priority
scheduling), 6 (register-file prefetch).  Same visual language as that figure:
white boxes, black outlines, numbered black discs, gold table outlines, magenta
for the value being produced in the example, red for update paths.

    python3 draw_arch.py            -> fig1_h2p_hbt.{svg,pdf,png} ... in this directory
"""
import glob
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Circle, FancyArrowPatch, FancyBboxPatch, Rectangle

OUT = Path(__file__).resolve().parent
for f in glob.glob("/home/lee/figure_style/fonts/*.[ot]tf"):
    font_manager.fontManager.addfont(f)
plt.rcParams["font.family"] = ["Pretendard", "DejaVu Sans"]
plt.rcParams["svg.fonttype"] = "none"
plt.rcParams["pdf.fonttype"] = 42

GOLD = "#F2B300"
GOLD_FILL = "#FFF3CC"
MAG = "#B5176A"
RED = "#E53935"
LIGHT = "#F4F4F4"
NOTE = "#555555"
FS = 8.2


def canvas(w=7.0, h=4.2):
    fig = plt.figure(figsize=(w, h))
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 100 * h / w)
    ax.axis("off")
    return fig, ax, 100 * h / w


def txt(ax, x, y, s, ha="center", va="center", fs=FS, bold=False, color="black", **kw):
    ax.text(x, y, s, ha=ha, va=va, fontsize=fs, color=color,
            fontweight="bold" if bold else "normal", linespacing=1.25, **kw)


def box(ax, x, y, w, h, s="", fs=FS, fc="white", ec="black", lw=1.1, bold=False,
        color="black", rounded=False, ls="-"):
    if rounded:
        p = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=1.2",
                           fc=fc, ec=ec, lw=lw, ls=ls)
    else:
        p = Rectangle((x, y), w, h, fc=fc, ec=ec, lw=lw, ls=ls)
    ax.add_patch(p)
    if s:
        txt(ax, x + w / 2, y + h / 2, s, fs=fs, bold=bold, color=color)


def num(ax, x, y, n, r=1.55):
    ax.add_patch(Circle((x, y), r, fc="black", ec="black"))
    txt(ax, x, y - 0.05, str(n), fs=FS - 0.6, bold=True, color="white")


def arrow(ax, p0, p1, color="black", lw=1.1, style="-|>", ls="-", ms=9):
    a = FancyArrowPatch(p0, p1, arrowstyle=style, mutation_scale=ms, color=color,
                        lw=lw, ls=ls, shrinkA=0, shrinkB=0)
    ax.add_patch(a)


def poly_arrow(ax, pts, color="black", lw=1.1, ls="-", ms=9):
    for (x0, y0), (x1, y1) in zip(pts[:-2], pts[1:-1]):
        ax.plot([x0, x1], [y0, y1], color=color, lw=lw, ls=ls, solid_capstyle="round")
    arrow(ax, pts[-2], pts[-1], color=color, lw=lw, ls=ls, ms=ms)


def line(ax, pts, color="black", lw=1.1, ls="-"):
    xs, ys = zip(*pts)
    ax.plot(xs, ys, color=color, lw=lw, ls=ls, solid_capstyle="round")


def table(ax, x, ytop, cols, rows, rowh=3.6, hl=(), title=None, fs=FS - 0.4, gold_cols=()):
    """cols: [(label, width)], rows: [[...]], hl: {(r, c)} magenta, gold_cols: outlined."""
    y = ytop
    if title:
        txt(ax, x + sum(w for _, w in cols) / 2, y + 2.3, title, fs=FS, bold=True)
    cx = x
    for label, w in cols:
        box(ax, cx, y - rowh, w, rowh, label, fs=fs, fc=LIGHT)
        cx += w
    y -= rowh
    for r, row in enumerate(rows):
        cx = x
        for c, ((_, w), val) in enumerate(zip(cols, row)):
            box(ax, cx, y - rowh, w, rowh, "")
            txt(ax, cx + w / 2, y - rowh / 2, str(val), fs=fs,
                color=MAG if (r, c) in hl else "black", bold=(r, c) in hl)
            cx += w
        y -= rowh
    cx = x
    for c, (_, w) in enumerate(cols):
        if c in gold_cols:
            ax.add_patch(Rectangle((cx, y), w, ytop - rowh - y, fc="none", ec=GOLD, lw=1.8))
        cx += w
    return y


def row_center(ytop, r, rowh=3.6):
    """y of the centre of data row r (0-based) in a table whose title-less top is ytop."""
    return ytop - rowh - rowh * (r + 0.5)


def save(fig, name):
    for ext in ("svg", "pdf", "png"):
        fig.savefig(OUT / f"{name}.{ext}", dpi=200 if ext == "png" else None, facecolor="white")
    plt.close(fig)


# ============================================================================
# Logic 1 -- which branch is H2P (Hard-to-predict Branch Table)
# ============================================================================
def fig1():
    fig, ax, H = canvas(7.0, 4.0)

    box(ax, 2, H - 9, 24, 5.5, "[0x0100] JNE   (branch B)", bold=True)
    box(ax, 2, H - 26, 14, 5, "Fetch / Decode", fc=LIGHT)
    box(ax, 2, H - 40, 14, 5, "Execute\n(branch resolves)", fc=LIGHT, fs=FS - 0.4)
    box(ax, 2, H - 54, 14, 5, "Commit", fc=LIGHT)

    cols = [("index", 8), ("tag", 11), ("ctr (3b)", 10), ("ctr > 1", 9)]
    rows = [["…", "", "", ""], ["0x100", "0x0100", "3", "H2P"], ["0x200", "0x0200", "1", "–"],
            ["0x334", "0x0334", "6", "H2P"], ["…", "", "", ""]]
    tx, ty = 60, H - 6
    bottom = table(ax, tx, ty, cols, rows, hl={(1, 2), (1, 3)}, title="HBT (1024 entries)", gold_cols=(2,))
    txt(ax, tx + 19, bottom - 2.2, "3-bit saturating counter per branch PC", fs=FS - 0.8, color=NOTE)
    y_b = row_center(ty, 1)

    # (1) execute: mispredict -> ctr++
    num(ax, 19, H - 37.5, 1)
    box(ax, 22, H - 41, 25, 7, "mispredicted ?\nyes → HBT[PC].ctr ++  (max 7)", fs=FS - 0.5)
    arrow(ax, (16, H - 37.5), (22, H - 37.5))
    poly_arrow(ax, [(47, H - 37.5), (52, H - 37.5), (52, y_b), (60, y_b)], color=RED)
    txt(ax, 48, H - 35.2, "update", fs=FS - 1, color=RED)

    # (2) fetch/decode: lookup -> H2P bit
    num(ax, 19, H - 23.5, 2)
    box(ax, 22, H - 27, 22, 7, "HBT[PC] lookup\nctr > 1  ⇒  H2P bit = 1", fs=FS - 0.4)
    arrow(ax, (16, H - 23.5), (22, H - 23.5))
    poly_arrow(ax, [(60, y_b + 1.2), (50, y_b + 1.2), (50, H - 21), (44, H - 21)])
    txt(ax, 47, H - 18.5, "read", fs=FS - 1)
    txt(ax, 33, H - 30.5, "the bit travels with the branch through the pipeline", fs=FS - 1.2, color=NOTE)

    # (3) decay
    num(ax, tx + 2, bottom - 8, 3)
    box(ax, tx + 4.5, bottom - 11.5, 33, 6.5, "every 50K retired instructions:\nall counters −1  (decay)", fs=FS - 0.4)
    arrow(ax, (tx + 19, bottom - 5), (tx + 19, bottom - 0.2), ls="--")

    # (4) commit: seed chain root
    num(ax, 19, H - 51.5, 4)
    box(ax, 22, H - 55, 22, 7, "H2P bit = 1 ?\nyes → insert PC as chain root", fs=FS - 0.4)
    arrow(ax, (16, H - 51.5), (22, H - 51.5))
    bcols = [("PC", 11), ("PCbr", 11), ("depth", 8)]
    bx, by = 60, bottom - 15.5
    table(ax, bx, by, bcols, [["0x0100", "0x0100", "0"]], hl={(0, 1), (0, 2)}, title="brslice_tab (chain root)")
    arrow(ax, (44, H - 51.5), (bx, row_center(by, 0)), color=RED)
    txt(ax, 52, H - 49.3, "seed", fs=FS - 1, color=RED)
    poly_arrow(ax, [(bx + 16.5, by - 7.2), (bx + 16.5, by - 9.5), (bx + 36, by - 9.5), (bx + 36, bottom - 0.2)], ls=":", ms=7)
    txt(ax, bx + 26, by - 11.5, "PCbr → HBT counter (owner is still H2P ?)", fs=FS - 1.4, color=NOTE)
    save(fig, "fig1_h2p_hbt")


# ============================================================================
# Logic 4 -- Target Load detection and address predictability (Prefetch Table)
# ============================================================================
def fig4():
    fig, ax, H = canvas(7.0, 4.3)

    box(ax, 2, H - 9, 32, 5.5, "[0x0020] L: LOAD p7 ← [p3 + 8]", bold=True)
    box(ax, 2, H - 24, 14, 5, "Rename", fc=LIGHT)
    box(ax, 2, H - 48, 14, 5, "Commit", fc=LIGHT)

    cols = [("tag", 9), ("base", 10), ("stride", 8), ("conf", 6), ("inflt", 6), ("util", 6)]
    rows = [["…", "", "", "", "", ""], ["0x0020", "0x7F10", "+8", "1", "2", "5"],
            ["0x0148", "0x2000", "+64", "0", "0", "1"], ["…", "", "", "", "", ""]]
    tx, ty = 52, H - 5
    bottom = table(ax, tx, ty, cols, rows, hl={(1, 1), (1, 2), (1, 3)},
                   title="Prefetch Table (1K entries, 8-way, load-PC indexed)", gold_cols=(1, 2, 3))

    # (3) rename: prediction (drawn first so the anchors read top-down)
    num(ax, 19, H - 21.5, 3)
    box(ax, 21, H - 27, 26, 11,
        "PT[PC] hit and conf saturated ?\npred = base + stride × inflt\ninflt ++\n→ RF prefetch launch (Logic 6)\nexample: 0x7F10 + 8 × 2 = 0x7F20", fs=FS - 0.9)
    arrow(ax, (16, H - 21.5), (21, H - 21.5))
    y_row = row_center(ty, 1)
    poly_arrow(ax, [(tx, y_row), (49.5, y_row), (49.5, H - 18), (47, H - 18)], ms=8)
    txt(ax, 49.5, y_row + 1.8, "read", fs=FS - 1)

    # dispatch bus from Commit to (2) and (1)
    line(ax, [(16, H - 45.5), (18.5, H - 45.5), (18.5, H - 34)])
    arrow(ax, (18.5, H - 34), (21, H - 34))
    arrow(ax, (18.5, H - 45.5), (21, H - 45.5))

    # (2) commit: stride training
    num(ax, 18.5, H - 34, 2)
    box(ax, 21, H - 39, 26, 10,
        "Δ = addr − base\nΔ == stride ? conf ++ (prob. 1/16)\n              : stride ← Δ, conf ← 0\nbase ← addr,  inflt −−", fs=FS - 0.9)
    poly_arrow(ax, [(47, H - 34), (tx + 14, H - 34), (tx + 14, bottom - 0.2)], color=RED)
    txt(ax, 50.5, H - 31.8, "train", fs=FS - 1, color=RED)

    # (1) commit: membership -> allocate/refresh
    num(ax, 18.5, H - 45.5, 1)
    box(ax, 21, H - 50, 26, 9,
        "brslice_tab[PC] member ?\nyes → PT allocate / refresh (util ++)\nno  → not a Target Load", fs=FS - 0.8)
    poly_arrow(ax, [(47, H - 45.5), (tx + 42, H - 45.5), (tx + 42, bottom - 0.2)], color=RED)
    txt(ax, 51.5, H - 43.3, "allocate", fs=FS - 1, color=RED)

    txt(ax, 2, H - 53.5,
        "predictable  =  the same stride on a long run of retires (1-bit confidence, raised with probability 1/16).\n"
        "Only chain-member loads are ever allocated, so the table holds Target Loads only; util picks the eviction victim.",
        ha="left", va="top", fs=FS - 1.3, color=NOTE)
    save(fig, "fig4_target_load_pt")


# ============================================================================
# Logic 5 -- priority scheduling (partitioned reservation station)
# ============================================================================
def fig5():
    fig, ax, H = canvas(7.0, 4.1)

    box(ax, 2, H - 9, 30, 5.5, "[0x0014] I3: ADD p3 ← p1 + p2", bold=True)
    box(ax, 2, H - 22, 14, 5, "Decode / Rename", fc=LIGHT, fs=FS - 0.6)
    box(ax, 2, H - 38, 14, 5, "Dispatch", fc=LIGHT)
    box(ax, 2, H - 55, 14, 5, "Select / Issue", fc=LIGHT, fs=FS - 0.4)

    # RS with two partitions
    rx, ry_top, rw, rh = 62, H - 6, 22, 3.2
    txt(ax, rx + rw / 2, ry_top + 2.4, "Reservation Station (each of RS0–RS2)", fs=FS, bold=True)
    cells = ["P   I3", "P   —", "I1", "—", "I2", "—", "I4", "—"]
    for i, c in enumerate(cells):
        box(ax, rx, ry_top - (i + 1) * rh, rw, rh, c, fs=FS - 0.6, fc=GOLD_FILL if i < 2 else "white")
    ax.add_patch(Rectangle((rx, ry_top - 2 * rh), rw, 2 * rh, fc="none", ec=GOLD, lw=1.8))
    txt(ax, rx + rw + 1, ry_top - rh, "priority\npartition (25%)", ha="left", fs=FS - 1.2)
    txt(ax, rx + rw + 1, ry_top - 5 * rh, "normal\npartition", ha="left", fs=FS - 1.2)
    rs_bottom = ry_top - 8 * rh
    y_pri, y_nrm = ry_top - rh, ry_top - 5 * rh

    # free lists
    fx, fw = 49, 9
    box(ax, fx, ry_top - 2 * rh, fw, 2 * rh, "priority\nfree list", fs=FS - 1.2, fc=GOLD_FILL)
    box(ax, fx, ry_top - 8 * rh, fw, 6 * rh, "normal\nfree list", fs=FS - 1.2)
    arrow(ax, (fx + fw, y_pri), (rx, y_pri), ms=7)
    arrow(ax, (fx + fw, y_nrm), (rx, y_nrm), ms=7)

    # (1) membership lookup -> priority bit
    num(ax, 19, H - 19.5, 1)
    box(ax, 21, H - 23, 24, 7, "brslice_tab[PC] member ?\nyes → priority bit = 1", fs=FS - 0.4)
    arrow(ax, (16, H - 19.5), (21, H - 19.5))
    txt(ax, 33, H - 25.3, "the bit travels with the op", fs=FS - 1.2, color=NOTE)

    # Dispatch bus feeding (2) and (3)
    line(ax, [(16, H - 35.5), (18.5, H - 35.5), (18.5, H - 45)])
    arrow(ax, (18.5, H - 35.5), (21, H - 35.5))
    arrow(ax, (18.5, H - 45), (21, H - 45))

    # (2) dispatch steering
    num(ax, 18.5, H - 35.5, 2)
    box(ax, 21, H - 39, 24, 7, "priority bit = 1 → priority free list\nelse             → normal free list", fs=FS - 0.9)
    line(ax, [(45, H - 35.5), (47, H - 35.5), (47, y_pri)])
    arrow(ax, (47, y_pri), (fx, y_pri), color=RED)
    line(ax, [(47, H - 35.5), (47, y_pri)], color=RED)
    arrow(ax, (47, y_nrm), (fx, y_nrm))

    # (3) fallback
    num(ax, 18.5, H - 45, 3)
    box(ax, 21, H - 48.5, 24, 7, "priority partition full →\nnon-stall fallback: take a normal\nentry, priority bit cleared for good", fs=FS - 1.1)

    # (4) select
    num(ax, 19, H - 52.5, 4)
    box(ax, 21, H - 57, 24, 8, "among ready entries:\npriority first,\nthen random-queue (baseline)", fs=FS - 0.9)
    arrow(ax, (16, H - 52.5), (21, H - 52.5))
    box(ax, rx, rs_bottom - 8, rw, 5, "select:  priority ?  ›  random", fs=FS - 1, rounded=True)
    for i in (0, 1, 2):
        arrow(ax, (rx + 5 + i * 6, rs_bottom), (rx + 5 + i * 6, rs_bottom - 3), ms=6)
    arrow(ax, (rx + rw, rs_bottom - 5.5), (rx + rw + 7, rs_bottom - 5.5))
    txt(ax, rx + rw + 7.5, rs_bottom - 5.5, "to FU port", ha="left", fs=FS - 1.2)
    poly_arrow(ax, [(45, H - 53), (50, H - 53), (50, rs_bottom - 5.5), (rx, rs_bottom - 5.5)], ms=7, ls="--")

    # (5) release
    num(ax, rx + 1.5, rs_bottom - 11.5, 5)
    txt(ax, rx + 4, rs_bottom - 11.5, "issued entry returns to its own free list", ha="left", fs=FS - 1.2, color=NOTE)
    save(fig, "fig5_priority_iq")


# ============================================================================
# Logic 6 -- register-file prefetch (RFP) timing and data path
# ============================================================================
def fig6():
    fig, ax, H = canvas(7.0, 4.6)

    stages = [("Rename", 2), ("Dispatch / RS", 20), ("AGU (load issues)", 40), ("L1D access", 62), ("Writeback", 82)]
    for name, x in stages:
        box(ax, x, H - 8, 16, 5, name, fc=LIGHT, fs=FS - 0.6)
    for (_, x0), (_, x1) in zip(stages[:-1], stages[1:]):
        arrow(ax, (x0 + 16, H - 5.5), (x1, H - 5.5), ms=7)
    txt(ax, 50, H - 1.8, "demand path of load L:   LOAD p7 ← [addr]", fs=FS - 0.6, color=NOTE)

    # PRF
    cols = [("preg", 7), ("value", 13), ("ready", 7)]
    px, py = 72, H - 30
    table(ax, px, py, cols, [["p6", "…", "1"], ["p7", "0x7F20 data", "1"], ["p8", "…", "0"]],
          hl={(1, 1), (1, 2)}, title="Physical Register File", gold_cols=(1,))
    y_p7 = row_center(py, 1)

    # (1) rename: launch
    num(ax, 4, H - 17, 1)
    box(ax, 6.5, H - 21.5, 28, 9, "Prefetch Table hit (Logic 4)\npacket = { dest preg p7, pred addr }\n→ prefetch queue", fs=FS - 0.8)
    box(ax, 6.5, H - 33, 28, 5, "prefetch queue  (64 entries, FIFO)", fs=FS - 0.8, fc=GOLD_FILL)
    txt(ax, 21.5, H - 34.8, "≤ 2 drains / cycle", ha="left", fs=FS - 1.4, color=NOTE)
    arrow(ax, (20.5, H - 21.5), (20.5, H - 28))

    # (2) store scan
    num(ax, 4, H - 40.5, 2)
    box(ax, 6.5, H - 44.5, 28, 8, "scan older stores with pred addr\nmatch → wait for that store,\ntake its data  (no L1 access)", fs=FS - 0.8)
    arrow(ax, (20.5, H - 33), (20.5, H - 36.5))
    poly_arrow(ax, [(34.5, H - 40.5), (60, H - 40.5), (60, y_p7 - 0.9), (px, y_p7 - 0.9)], color=RED, ls="--")
    txt(ax, 47, H - 38.3, "store data → p7", fs=FS - 1, color=RED)

    # (3) L1 probe on a spare port
    num(ax, 4, H - 52, 3)
    box(ax, 6.5, H - 56, 28, 8, "no match → L1D read on a spare port\n(demand loads first)\nhit: data → p7 ;  miss: fill from L2 → p7", fs=FS - 0.9)
    arrow(ax, (20.5, H - 44.5), (20.5, H - 48))
    poly_arrow(ax, [(34.5, H - 52), (64, H - 52), (64, y_p7 + 0.9), (px, y_p7 + 0.9)], color=RED)
    txt(ax, 49, H - 49.8, "prefetched data → p7", fs=FS - 1, color=RED)
    txt(ax, 2, H - 59, "Launched right after rename, as in the RFP paper: the destination register already exists\n"
                       "and the older-store logic of the load pipeline is reused, so a correct address implies correct data.",
        ha="left", va="top", fs=FS - 1.3, color=NOTE)

    # (4) validation at AGU
    num(ax, 36.5, H - 14, 4)
    box(ax, 38.5, H - 27, 32, 13,
        "load computes its address\naddr == pred  and  no older-store conflict ?\nyes → skip the L1 access; done at p7-ready + 1,\n        dependents wake from the PRF\nno  → normal L1D access", fs=FS - 1.3)
    arrow(ax, (48, H - 8), (48, H - 14), ms=7)
    poly_arrow(ax, [(70.5, H - 24), (71.3, H - 24), (71.3, y_p7 + 1.6), (px, y_p7 + 1.6)], ms=7, ls=":")
    txt(ax, 65, H - 29, "p7 ready ?", fs=FS - 1.3, color=NOTE)
    save(fig, "fig6_rfp")


# ============================================================================
# Logic 3 -- slice membership at uop granularity (brslice_tab entry layout)
# ============================================================================
def fig7():
    fig, ax, H = canvas(7.0, 3.7)

    # --- the instruction and the uops it decodes into ------------------------
    txt(ax, 2, H - 3, "x86 instruction", fs=FS, bold=True, ha="left")
    box(ax, 2, H - 10, 30, 5.5, "[0x0340]  add rax, [rbx]", bold=True)
    txt(ax, 2, H - 13.5, "two uops, both carrying PC 0x0340", fs=FS - 1.2, color=NOTE, ha="left")
    box(ax, 2, H - 23, 14, 6, "uop 0\nload", fs=FS - 0.6)
    box(ax, 17, H - 23, 14, 6, "uop 1\nadd", fs=FS - 0.6)
    arrow(ax, (17, H - 10), (9, H - 17))
    arrow(ax, (17, H - 10), (24, H - 17))
    txt(ax, 9, H - 25.3, "index 0", fs=FS - 1.4, color=NOTE)
    txt(ax, 24, H - 25.3, "index 1", fs=FS - 1.4, color=NOTE)

    # --- the table ----------------------------------------------------------
    cols = [("tag (PC)", 12), ("uop mask", 10), ("owner (H2P)", 13), ("depth", 8)]
    rows = [["0x0288", "0 0 1 0", "0x0100", "2"],
            ["0x0340", "1 0 0 0", "0x0100", "1"],
            ["0x03C4", "1 1 0 0", "0x0334", "3"]]
    tx, ty = 55, H - 4
    bottom = table(ax, tx, ty, cols, rows, hl={(1, 0), (1, 1)},
                   title="brslice_tab   (PC-indexed, one bit per uop)", gold_cols=(1,))
    y_e = row_center(ty, 1)

    # --- (1) one lookup, at decode ------------------------------------------
    num(ax, 35.5, H - 20, 1)
    box(ax, 38, H - 24, 13, 8, "lookup\nbrslice_tab[PC]", fs=FS - 0.5)
    arrow(ax, (31, H - 20), (38, H - 20))
    poly_arrow(ax, [(51, H - 20), (53, H - 20), (53, y_e), (tx, y_e)])
    txt(ax, 52.2, H - 16.5, "PC", fs=FS - 1.2)

    # --- (2) each uop takes its own bit -------------------------------------
    poly_arrow(ax, [(tx, y_e - 1.4), (53, y_e - 1.4), (53, H - 30), (45.5, H - 30)], color=MAG)
    txt(ax, 49.5, H - 27.7, "mask", fs=FS - 1.2, color=MAG)
    num(ax, 4, H - 30, 2)
    box(ax, 6.5, H - 33.5, 39, 7,
        "uop i takes bit i of the mask\nuop 0 (load) = 1 → priority      uop 1 (add) = 0 → normal",
        fs=FS - 0.7)

    # --- (3) the bit rides down the pipeline --------------------------------
    num(ax, 4, H - 42, 3)
    box(ax, 6.5, H - 45.5, 39, 7,
        "the 1-bit result rides with the uop\nrename → P-IQ admission → select", fs=FS - 0.7, fc=LIGHT)
    arrow(ax, (26, H - 33.5), (26, H - 38.5))

    # --- (4) fill path ------------------------------------------------------
    num(ax, 56.5, H - 24.5, 4)
    box(ax, 59, H - 28, 36, 7,
        "at commit the backward walk sets the bit\nof the uop it reached, not of the whole PC", fs=FS - 0.7)
    arrow(ax, (77, H - 21), (77, bottom - 0.4), color=RED, ls="--")
    txt(ax, 81.5, H - 19.7, "update", fs=FS - 1.2, color=RED)

    txt(ax, 50, 3.2, "bit i = uop i of that macro-op is on the critical slice;  four bits cover almost every x86 "
                     "instruction", fs=FS - 1.1, color=NOTE)
    save(fig, "fig7_brslice_tab_uop")


if __name__ == "__main__":
    fig1()
    fig7()
    fig4()
    fig5()
    fig6()
    print("wrote", sorted(p.name for p in OUT.glob("fig*_*.svg")))

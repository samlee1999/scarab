#!/usr/bin/env python3
"""Illustrative cycle-by-cycle example (not a simulator trace): the H2P branch of GAP sssp's edge relaxation, before
and after CLEAR, and with a PUBS-style full slice.

Code: RelaxEdges() inner loop, x86-64 from the SimPoint 3993 trace (PC 0x7ffff3daa651..68f), simplified -- the binary
reloads &dist and u from the stack every iteration; here they live in rsi and r12.

    old_dist = dist[wn.v]          path A (critical):     A1 add rbx -> A2 movsxd wn.v -> A3 lea -> A4 ld dist[wn.v]
    new_dist = dist[u] + wn.w      path B (not critical): B1 mov dist[u], B2 ld wn.w -> B2 add
    while (new_dist < old_dist)    A4 cmp (the two paths meet) -> jle (H2P)

Timing rules -- the same as the user's generic example figure (2026-10-06):
  * every micro-op: W (wakeup) -> S (select) -> R (register read) -> E (ALU / branch) or A (AGU) + C x 4 (L1 access)
  * a consumer wakes at its producer's S + latency - 1 (ALU 1, L1-hit load 5), so it executes back to back
  * s = select wait (contention with other ready micro-ops); a Priority IQ entry removes it
  * RF prefetch: the value is already in the PRF, so the L1 access is skipped (dashed) and the load counts as 1 cycle
  * dist[wn.v] is a random access into a 2 MB array: it misses L1 and hits L2 (16 cycles load-to-use) and cannot be
    RF-prefetched (no stride).  dist[u] (fixed address) and wn.v / wn.w (stride 8) can.
Which micro-ops each design accelerates follows the measured per-PC ledger of SimPoint 3993, idealized: CLEAR follows
only the last-arriving edge, so path B stays out (in the run CLEAR also marked B2, because the compare's inputs are exempt
from filter A and B2 arrived last on 0.42% of instances); the full slice marks every producer.
Output: fig_sssp_cycle_example.{pdf,svg} (+ _preview.png), full width.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, '/home/lee')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle

from figure_style import BLUE_5, FULL_WIDTH_PT, configure_style, paper_figsize, save_paper_figure

STEM = 'fig_sssp_cycle_example'
SIZE, HEIGHT_PT = 'full', 297.0
MONO_FILES = ['/usr/share/fonts/truetype/msttcorefonts/Courier_New.ttf',
              '/usr/share/fonts/truetype/msttcorefonts/Courier_New_Bold.ttf']

# ---------------------------------------------------------------------------------------------------------------------
# Micro-ops of one iteration: key, row label, path, kind, producers.  Rows top to bottom.
UOPS = [
    ('A1', 'A1 add', 'A', 'alu', []),
    ('A2', 'A2 movsxd', 'A', 'load', ['A1']),          # wn.v, stride 8
    ('A3', 'A3 lea', 'A', 'alu', ['A2']),
    ('A4l', 'A4 ld', 'A', 'miss', ['A3']),             # dist[wn.v]: random -> L1 miss, no stride
    ('A4c', 'A4 cmp', 'M', 'alu', ['A4l', 'B2a']),     # the two paths meet
    ('BR', 'jle', 'H', 'br', ['A4c']),
    ('B1', 'B1 mov', 'B', 'load', []),                 # dist[u], fixed address
    ('B2l', 'B2 ld', 'B', 'load', ['A1']),             # wn.w, stride 8
    ('B2a', 'B2 add', 'B', 'alu', ['B1', 'B2l']),
]
KIND = {k: kind for k, _, _, kind, _ in UOPS}
DEPS = {k: deps for k, _, _, _, deps in UOPS}
ORDER = ['A1', 'B1', 'A2', 'B2l', 'A3', 'B2a', 'A4l', 'A4c', 'BR']   # producers before consumers
# Select waits without a Priority IQ entry (illustrative contention, as in the generic example).
WAITS = {'A1': 1, 'A2': 1, 'A3': 0, 'A4l': 1, 'A4c': 0, 'BR': 1, 'B1': 2, 'B2l': 1, 'B2a': 1}
L1_CYCLES, L2_EXTRA = 4, 11                        # load-to-use: L1 hit 1 + 4 = 5, L2 hit 5 + 11 = 16
PATH_A = {'A1', 'A2', 'A3', 'A4l', 'A4c', 'BR'}
CONFIGS = {   # name: (micro-ops with a Priority IQ entry, loads RF-prefetched)
    'base': (set(), set()),
    'clear': (PATH_A, {'A2'}),
    'clear_piq': (PATH_A, set()),
    'clear_rfp': (set(), {'A2'}),
    'full': (set(KIND), {'A2', 'B1', 'B2l'}),
}


def latency(k: str, rfp: set) -> int:
    if KIND[k] in ('alu', 'br') or k in rfp:
        return 1
    return 1 + L1_CYCLES + (L2_EXTRA if KIND[k] == 'miss' else 0)


def schedule(cfg: str) -> dict:
    prio, rfp = CONFIGS[cfg]
    t = {}
    for k in ORDER:
        w = max([0] + [t[p]['S'] + latency(p, rfp) - 1 for p in DEPS[k]])
        waits = 0 if k in prio else WAITS[k]
        s = w + 1 + waits
        t[k] = {'W': w, 'waits': waits, 'S': s, 'R': s + 1, 'X': s + 2, 'prio': k in prio, 'rfp': k in rfp}
    return t


def resolve(cfg: str) -> int:
    return schedule(cfg)['BR']['X']


# ---------------------------------------------------------------------------------------------------------------------
GREEN, PURPLE, RED, INK, GRAY = '#1a8f3c', '#7b4fa6', '#d62728', '#222222', '#6d6d6d'
PATH_COLOR = {'A': GREEN, 'B': PURPLE, 'M': INK, 'H': RED}
STAGE_FILL, STAGE_EDGE, STAGE_TEXT = '#efefef', '#9a9a9a', '#4d4d4d'
AGU_FILL, EXE_FILL = '#cfe0f0', '#c4d6c4'
WAIT_FILL, L1_FILL, L2_FILL, BR_FILL = '#e8912d', BLUE_5, '#5f5f5f', '#1b2f4e'
GRID_LINE = '#e6e6e6'
XMAX = 36
BOX_W, BOX_H = 0.88, 0.62
BOX_FONT, LABEL_FONT, TITLE_FONT, NOTE_FONT, TICK_FONT = 4.8, 6.0, 7.0, 5.6, 6.0

# Layout in points (full width x HEIGHT_PT).
LABEL_W, PANEL_W, PANEL_H = 48.0, 205.0, 90.0
LEFT_X, RIGHT_X = LABEL_W, LABEL_W + PANEL_W + 52.0
BOTTOM_Y = 48.0                      # (b), (c): axes bottom
TOP_Y = BOTTOM_Y + PANEL_H + 46.0    # (a): axes bottom (title block of (b) + gap + tick labels of (a))


def mono(size: float = LABEL_FONT) -> font_manager.FontProperties:
    return font_manager.FontProperties(fname=MONO_FILES[0], size=size)


def axes_pt(fig, x: float, y: float, w: float, h: float):
    return fig.add_axes([x / FULL_WIDTH_PT, y / HEIGHT_PT, w / FULL_WIDTH_PT, h / HEIGHT_PT])


def box(ax, x: float, y: float, letter: str, fill: str, *, edge: str = STAGE_EDGE, text: str = STAGE_TEXT,
        width: float = 1.0, dashed: bool = False) -> None:
    ax.add_patch(Rectangle((x + (1 - BOX_W) / 2, y - BOX_H / 2), width - (1 - BOX_W), BOX_H, facecolor=fill,
                           edgecolor=edge, linewidth=0.45, linestyle=(0, (1.6, 1.2)) if dashed else '-', zorder=3))
    ax.text(x + width / 2, y, letter, ha='center', va='center', fontsize=BOX_FONT, color=text, zorder=4)


def draw_panel(ax, cfg: str, title: str, note: str) -> None:
    t = schedule(cfg)
    rows = {k: i for i, (k, *_) in enumerate(UOPS)}
    for x in range(XMAX + 1):
        ax.axvline(x, color=GRID_LINE, linewidth=0.35, zorder=0)
    ax.axhline(rows['BR'] + 0.5, color='#bdbdbd', linewidth=0.5, linestyle=(0, (2, 2)), zorder=1)

    for k, label, path, kind, _ in UOPS:
        y, r = rows[k], t[k]
        ax.text(-1.35, y, label, ha='right', va='center', fontproperties=mono(), color=PATH_COLOR[path],
                clip_on=False)
        if r['prio']:   # carries the priority bit: a Priority IQ entry
            ax.plot(-0.6, y, marker='o', markersize=2.4, color=INK, clip_on=False, zorder=5)
        box(ax, r['W'], y, 'W', STAGE_FILL)
        for i in range(r['waits']):
            box(ax, r['W'] + 1 + i, y, 's', WAIT_FILL, edge=WAIT_FILL, text='white')
        box(ax, r['S'], y, 'S', STAGE_FILL)
        box(ax, r['R'], y, 'R', STAGE_FILL)
        x = r['X']
        if kind == 'alu':
            box(ax, x, y, 'E', EXE_FILL)
        elif kind == 'br':
            box(ax, x, y, 'E', BR_FILL, edge=BR_FILL, text='white')
        else:
            box(ax, x, y, 'A', AGU_FILL)
            for i in range(L1_CYCLES):
                if r['rfp']:
                    box(ax, x + 1 + i, y, '·', 'white', edge=L1_FILL, text=L1_FILL, dashed=True)
                else:
                    box(ax, x + 1 + i, y, 'C', L1_FILL, edge=L1_FILL, text='white')
            if kind == 'miss':
                box(ax, x + 1 + L1_CYCLES, y, 'L1 miss → L2', L2_FILL, edge=L2_FILL, text='white', width=L2_EXTRA)

    # The H2P branch resolves at its E; dist[wn.v] issues at its S; new_dist waits for the compare (slack).
    res = t['BR']['X']
    ax.axvline(res + 1, color=RED, linewidth=0.9, linestyle=(0, (3, 2)), zorder=2)
    ax.text(res + 1.3, rows['BR'], f'resolves\n@ {res}', ha='left', va='center', fontsize=NOTE_FONT, color=RED,
            fontweight='bold', linespacing=0.95)
    s_ld = t['A4l']['S']
    ax.plot(s_ld + 0.5, rows['A4l'] + 0.5, marker='^', markersize=2.4, color=INK, zorder=5)
    ax.text(s_ld + 0.95, rows['A4l'] + 0.56, f'dist[wn.v] issues @ {s_ld}', ha='left', va='center', fontsize=5.0,
            color=INK, zorder=5)
    ready, used = t['B2a']['X'] + 1, t['A4c']['X']
    y = rows['B2a']
    ax.annotate('', xy=(used, y), xytext=(ready + 0.1, y),
                arrowprops=dict(arrowstyle='<->', color='#8c8c8c', linewidth=0.6, shrinkA=0, shrinkB=0), zorder=2)
    ax.text((ready + used) / 2, y, f'slack {used - ready}', ha='center', va='center', fontsize=5.0, color=GRAY,
            zorder=2, bbox=dict(facecolor='white', edgecolor='none', pad=0.6))

    ax.set_xlim(-0.2, XMAX)
    ax.set_ylim(len(UOPS) - 0.5, -0.5)
    ax.set_xticks(range(0, XMAX + 1, 5))
    ax.tick_params(axis='x', labelsize=TICK_FONT, pad=1.0, length=0)
    ax.set_yticks([])
    for side in ('left', 'right', 'top'):
        ax.spines[side].set_visible(False)
    ax.spines['bottom'].set_linewidth(0.5)
    ax.spines['bottom'].set_color('#9a9a9a')
    ax.annotate(title, xy=(0, 1), xycoords='axes fraction', xytext=(-LABEL_W + 2, 10.5), textcoords='offset points',
                ha='left', va='bottom', fontsize=TITLE_FONT, fontweight='bold', color=INK)
    ax.annotate(note, xy=(0, 1), xycoords='axes fraction', xytext=(-LABEL_W + 2, 2.0), textcoords='offset points',
                ha='left', va='bottom', fontsize=NOTE_FONT, color=GRAY)


LISTING = [   # label, code, comment, path
    ('', '.loop:', '', 'I'),
    ('A1', 'add    rbx, 8', 'wn++', 'A'),
    ('I1', 'cmp    rbx, r13', 'end of list?', 'I'),
    ('I2', 'je     .done', '', 'I'),
    ('B1', 'mov    eax, [rsi+r12*4]', 'dist[u]', 'B'),
    ('I3', 'mov    rdi, r8', 'local_bins.size()', 'I'),
    ('A2', 'movsxd rcx, [rbx]', 'wn.v', 'A'),
    ('B2', 'add    eax, [rbx+4]', 'new = dist[u] + wn.w', 'B'),
    ('I4', 'sub    rdi, r14', '', 'I'),
    ('I5', 'sar    rdi, 3', '', 'I'),
    ('A3', 'lea    rdx, [rsi+rcx*4]', '&dist[wn.v]', 'A'),
    ('I6', 'imul   rdi, r15', '', 'I'),
    ('A4', 'cmp    [rdx], eax', 'old = dist[wn.v]', 'A'),
    ('', 'jle    .loop', 'H2P: new < old ?', 'H'),
]
LIST_COLOR = {'A': GREEN, 'B': PURPLE, 'H': RED, 'I': INK}


def draw_listing(ax) -> None:
    """Axes in points: x from the left edge of the cell, y down from its top."""
    ax.set_axis_off()
    w, h = ax.bbox.width * 72 / ax.figure.dpi, ax.bbox.height * 72 / ax.figure.dpi
    ax.set_xlim(0, w)
    ax.set_ylim(h, 0)
    ax.text(0, 4, 'GAP sssp: RelaxEdges() inner loop, x86-64 (simplified)', ha='left', va='center',
            fontsize=TITLE_FONT, fontweight='bold', color=INK)
    pitch, y0 = 7.35, 14.5
    for i, (label, code, comment, path) in enumerate(LISTING):
        y = y0 + i * pitch
        c = LIST_COLOR[path]
        ax.text(0, y, label, ha='left', va='center', fontproperties=mono(6.6), color=c)
        ax.text(15, y, code, ha='left', va='center', fontproperties=mono(6.6), color=c)
        if comment:
            ax.text(118, y, '; ' + comment, ha='left', va='center', fontproperties=mono(6.0),
                    color=GRAY if path == 'I' else c)
    y = y0 + len(LISTING) * pitch + 3.5
    for x, text, c in [(0, 'path A: critical', GREEN), (72, 'path B: not critical', PURPLE),
                       (160, 'H2P branch', RED)]:
        ax.text(x, y, text, ha='left', va='center', fontsize=NOTE_FONT + 0.4, color=c, fontweight='bold')


def draw_legend(fig) -> None:
    """Two rows under the panels; each item placed after the measured end of the previous one."""
    ax = fig.add_axes([0, 0, 1, 1], zorder=-1)
    ax.set_axis_off()
    ax.set_xlim(0, FULL_WIDTH_PT)
    ax.set_ylim(0, HEIGHT_PT)
    renderer = fig.canvas.get_renderer()
    rows = [
        [('s', WAIT_FILL, 'white', False, 'select wait — removed by a Priority IQ entry'),
         ('C', L1_FILL, 'white', False, 'L1 access — removed by RF prefetch'),
         ('·', 'white', L1_FILL, True, 'L1 access skipped (value already in PRF)')],
        [('', L2_FILL, 'white', False, 'L1 miss → L2 (random address: no RF prefetch)'),
         ('dot', None, None, False, 'has a Priority IQ entry'),
         ('', None, None, False, 'W wakeup · S select · R reg read · A AGU · E execute')],
    ]
    for y, items in zip((17.0, 7.0), rows):
        x = LEFT_X - LABEL_W + 2
        for letter, fill, text, dashed, desc in items:
            if letter == 'dot':
                ax.plot(x + 3, y, marker='o', markersize=2.4, color=INK)
                tx = x + 8
            elif fill is not None:
                ax.add_patch(Rectangle((x, y - 3.2), 6.4, 6.4, facecolor=fill, edgecolor=text if dashed else fill,
                                       linewidth=0.45, linestyle=(0, (1.6, 1.2)) if dashed else '-'))
                ax.text(x + 3.2, y, letter, ha='center', va='center', fontsize=BOX_FONT, color=text)
                tx = x + 9
            else:
                tx = x
            art = ax.text(tx, y, desc, ha='left', va='center', fontsize=NOTE_FONT, color=INK)
            x = art.get_window_extent(renderer).x1 * 72 / fig.dpi + 14


def main() -> None:
    for cfg in CONFIGS:
        t = schedule(cfg)
        print(f'{cfg:10} resolves @ {resolve(cfg):2d}   dist[wn.v] issues @ {t["A4l"]["S"]:2d}   '
              f'new_dist ready @ {t["B2a"]["X"] + 1:2d}, used @ {t["A4c"]["X"]:2d}   '
              f'priority {sum(v["prio"] for v in t.values())}, RF prefetch {sum(v["rfp"] for v in t.values())}')
    base, clear, full = resolve('base'), resolve('clear'), resolve('full')
    piq, rfp = base - resolve('clear_piq'), base - resolve('clear_rfp')
    assert piq + rfp == base - clear, 'the two mechanisms should add up in this example'
    assert full == clear, 'accelerating path B should not move the branch'

    for f in MONO_FILES:
        font_manager.fontManager.addfont(f)
    configure_style()
    fig = plt.figure(figsize=paper_figsize(SIZE, height_pt=HEIGHT_PT))
    ax_a = axes_pt(fig, LEFT_X, TOP_Y, PANEL_W, PANEL_H)
    ax_b = axes_pt(fig, LEFT_X, BOTTOM_Y, PANEL_W, PANEL_H)
    ax_c = axes_pt(fig, RIGHT_X, BOTTOM_Y, PANEL_W, PANEL_H)
    ax_code = axes_pt(fig, RIGHT_X - LABEL_W + 2, TOP_Y - 14, FULL_WIDTH_PT - (RIGHT_X - LABEL_W + 2) - 2,
                      HEIGHT_PT - (TOP_Y - 14) - 2)
    count = lambda cfg: (sum(v['prio'] for v in schedule(cfg).values()), sum(v['rfp'] for v in schedule(cfg).values()))
    draw_panel(ax_a, 'base', f'(a) Baseline — resolves @ {base}', 'no Priority IQ entry, no RF prefetch')
    n_p, n_r = count('clear')
    draw_panel(ax_b, 'clear', f'(b) CLEAR: path A only — @ {clear}  (−{base - clear} = −{piq} P-IQ − {rfp} RFP)',
               f'Priority IQ entries: {n_p} µops · RF prefetch: {n_r} load')
    n_p, n_r = count('full')
    draw_panel(ax_c, 'full', f'(c) Full slice: paths A + B — @ {full}  (−{base - full})',
               f'Priority IQ entries: {n_p} µops · RF prefetch: {n_r} loads')
    ax_b.set_xlabel('cycle', fontsize=TICK_FONT, labelpad=0.5)
    ax_c.set_xlabel('cycle', fontsize=TICK_FONT, labelpad=0.5)
    draw_listing(ax_code)
    draw_legend(fig)
    fig.savefig(HERE / f'{STEM}_preview.png', dpi=300, facecolor='white')
    save_paper_figure(fig, HERE / f'{STEM}.svg', size=SIZE, height_pt=HEIGHT_PT, close=False)
    save_paper_figure(fig, HERE / f'{STEM}.pdf', size=SIZE, height_pt=HEIGHT_PT)
    print(HERE / f'{STEM}.pdf')


if __name__ == '__main__':
    main()

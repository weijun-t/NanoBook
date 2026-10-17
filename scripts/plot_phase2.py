"""plot_phase2.py: draws the phase 2 report's charts from the recorded benchmark numbers.

What it does: Reads the CSV files in docs/data/ and writes four PNG charts to docs/img/:
  phase2_times.png    time per operation, one small panel per benchmark, three books each
  phase2_speedup.png  how many times faster than the reference book, per benchmark
  phase2_allocs.png   heap allocations per added order, at each step of phase 2
  phase2_search.png   the search experiment inside the sorted levels (hybrid vs binary)
  Usage: .venv/bin/python scripts/plot_phase2.py

Why it exists: README §7.5: the plots read only CSV and never touch the engine, so they can be
  redrawn at any time from the numbers recorded in docs/perf-log.md.

How it works: matplotlib, one figure per chart. Times differ by up to 600x between benchmarks
  (8 ns to 5 µs), so instead of one chart with a log scale, each benchmark gets its own panel and
  its own linear scale ("small multiples"). Every bar carries its value as a label, so no number
  depends on reading a colour or an axis.

Trade-offs: Colours are the first three slots of the dataviz reference palette (blue, orange,
  aqua), which that palette documents as safe for colour-blind readers in every pair. Light mode
  only, because the charts go into a printed PDF. Needs matplotlib (requirements.txt).
"""

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402  (must come after choosing the backend)

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "docs" / "data"
OUT = ROOT / "docs" / "img"

# Reference palette (dataviz skill): surface, ink and the first three categorical slots.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BOOK_COLOURS = {"ReferenceBook": "#2a78d6", "FastBook": "#eb6834", "TickFastBook": "#1baf7a"}
BOOKS = list(BOOK_COLOURS)  # fixed order: the colour follows the book, never its rank
SINGLE = "#2a78d6"          # one-series charts use slot 1

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 8.5,
    "axes.facecolor": SURFACE,
    "figure.facecolor": SURFACE,
    "axes.edgecolor": BASELINE,
    "axes.labelcolor": INK_2,
    "xtick.color": INK_2,
    "ytick.color": MUTED,
    "axes.titlecolor": INK,
    "axes.titlesize": 9,
    "axes.titleweight": "bold",
})


def read(name: str) -> list[dict]:
    with open(DATA / name, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def tidy(ax) -> None:
    """Recessive chrome: no box, a hairline horizontal grid behind the bars, a quiet baseline."""
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    ax.tick_params(axis="x", length=0)


def fmt_ns(v: float) -> str:
    return f"{v / 1000:.2f} µs" if v >= 1000 else (f"{v:.1f} ns" if v < 100 else f"{v:.0f} ns")


def bars(ax, labels, values, colours, fmt) -> None:
    """Bars with a 2px gap (a surface-coloured edge) and the value printed above each one."""
    xs = range(len(values))
    ax.bar(xs, values, width=0.78, color=colours, edgecolor=SURFACE, linewidth=2)
    top = max(values) if values else 1
    for x, v in zip(xs, values):
        ax.text(x, v + top * 0.03, fmt(v), ha="center", va="bottom", fontsize=7.5, color=INK)
    ax.set_xticks(list(xs), labels)
    ax.set_ylim(0, top * 1.22)


def legend_strip(fig) -> None:
    handles = [plt.Rectangle((0, 0), 1, 1, color=BOOK_COLOURS[b]) for b in BOOKS]
    fig.legend(handles, BOOKS, loc="upper center", ncol=3, frameon=False,
               bbox_to_anchor=(0.5, 0.995), fontsize=8.5, labelcolor=INK_2)


# ---------------------------------------------------------------------------------------------

def plot_times() -> None:
    rows = read("phase2_final.csv")
    by_bench = defaultdict(dict)
    for r in rows:
        by_bench[r["benchmark"]][r["book"]] = float(r["time_ns"])
    benches = list(dict.fromkeys(r["benchmark"] for r in rows))  # keep the CSV's order
    cols = 5
    fig, axes = plt.subplots(2, cols, figsize=(10, 4.6))
    for ax, bench in zip(axes.flat, benches):
        vals = [by_bench[bench][b] for b in BOOKS]
        bars(ax, ["Ref", "Fast", "Tick"], vals, [BOOK_COLOURS[b] for b in BOOKS], fmt_ns)
        ax.set_title(bench, loc="left")
        ax.set_yticks([])
        tidy(ax)
    for ax in list(axes.flat)[len(benches):]:
        ax.axis("off")
    legend_strip(fig)
    fig.text(0.5, 0.005, "Time per operation (lower is better). Each panel has its own scale.",
             ha="center", fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.03, 1, 0.94))
    fig.savefig(OUT / "phase2_times.png", dpi=200)
    plt.close(fig)


def plot_speedup() -> None:
    rows = read("phase2_final.csv")
    t = defaultdict(dict)
    for r in rows:
        t[r["benchmark"]][r["book"]] = float(r["time_ns"])
    benches = list(dict.fromkeys(r["benchmark"] for r in rows))
    fig, ax = plt.subplots(figsize=(10, 4.2))
    width = 0.38
    for k, book in enumerate(["FastBook", "TickFastBook"]):
        ys = [i + (k - 0.5) * width for i in range(len(benches))]
        vals = [t[b]["ReferenceBook"] / t[b][book] for b in benches]
        ax.barh(ys, vals, height=width, color=BOOK_COLOURS[book], edgecolor=SURFACE,
                linewidth=2, label=book)
        for y, v in zip(ys, vals):
            ax.text(v + 0.08, y, f"{v:.1f}×", va="center", fontsize=7.5, color=INK)
    ax.axvline(1.0, color=INK_2, linewidth=1)
    ax.text(1.0, len(benches) - 0.35, " reference = 1×", fontsize=7.5, color=INK_2)
    ax.set_yticks(range(len(benches)), benches)
    ax.invert_yaxis()
    ax.set_xlabel("Times faster than the reference book (higher is better)")
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.xaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)
    ax.legend(loc="lower right", frameon=False, fontsize=8.5, labelcolor=INK_2)
    fig.tight_layout()
    fig.savefig(OUT / "phase2_speedup.png", dpi=200)
    plt.close(fig)


def plot_allocs() -> None:
    rows = read("phase2_allocs.csv")
    labels = [r["stage"].replace("\\n", "\n") for r in rows]
    vals = [float(r["allocs_per_add"]) for r in rows]
    fig, ax = plt.subplots(figsize=(8, 3.2))
    bars(ax, labels, vals, [SINGLE] * len(vals), lambda v: f"{v:.2f}")
    ax.set_ylabel("Heap allocations per added order")
    ax.set_yticks([])
    tidy(ax)
    fig.tight_layout()
    fig.savefig(OUT / "phase2_allocs.png", dpi=200)
    plt.close(fig)


def plot_search() -> None:
    rows = read("phase2_search.csv")
    by_bench = defaultdict(list)
    for r in rows:
        by_bench[r["benchmark"]].append((r["variant"], float(r["time_ns"])))
    fig, axes = plt.subplots(1, len(by_bench), figsize=(10, 3.2))
    for ax, (bench, items) in zip(axes, by_bench.items()):
        labels = [v.replace(" ", "\n") for v, _ in items]
        vals = [t for _, t in items]
        bars(ax, labels, vals, [SINGLE] * len(vals), fmt_ns)
        ax.set_title(bench, loc="left")
        ax.set_yticks([])
        tidy(ax)
    fig.text(0.5, 0.01, "Time per operation (lower is better). One-by-one checks from the best price "
             "before binary search; 0 = binary search only.", ha="center", fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    fig.savefig(OUT / "phase2_search.png", dpi=200)
    plt.close(fig)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    plot_times()
    plot_speedup()
    plot_allocs()
    plot_search()
    print("wrote", ", ".join(sorted(p.name for p in OUT.glob("phase2_*.png"))))


if __name__ == "__main__":
    main()

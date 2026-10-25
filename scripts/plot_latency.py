"""plot_latency.py: draws the latency harness's results (percentile curves and histograms).

What it does: Reads the CSV files nanobook_bench writes (--csv FILE.csv gives FILE.csv and
  FILE_histogram.csv) and draws two PNG charts:
  <prefix>_percentiles.png  one panel per scenario: latency at p50, p90, p99, p99.9 (and p99.99
                            when there were enough samples), one line per book
  <prefix>_histograms.png   one panel per scenario: how the samples are spread over time, one line
                            per book
  Usage: .venv/bin/python scripts/plot_latency.py FILE.csv [--out-dir DIR] [--prefix NAME]
  (defaults: the CSV's own folder, prefix "latency").

Why it exists: plan §7.5: the percentile curve is the headline chart, and the plots read only
  CSV, never the engine. Every chart carries the environment it was measured in (rule 19), and a
  NOT FOR PUBLISHING banner if the CSV came from a virtual machine (rule 20).

How it works:
  - Percentile curve: the x axis is spaced by "number of nines" (p90 = 1, p99 = 2, p99.9 = 3,
    p99.99 = 4; p50 sits at 0.3), so each step right is ten times rarer. Mixed runs use their
    "all" row, single-kind runs their one tag.
  - Histogram: the harness's buckets (1 ns wide at the bottom, ~1.6% wide above 1,024 ns) are
    regrouped into 10 bins per factor of ten, so every bin has the same width on the log time
    axis, and each bin shows the share of samples in it.

Trade-offs: Both charts use log scales, against the usual preference for small multiples. Latency
  is the exception: one panel spans about 10 ns to 100 µs, and the slow tail is the point, so a
  linear axis would squash everything but the largest values. Colours are the first three slots
  of the dataviz reference palette, as in plot_phase2.py. Needs matplotlib (requirements.txt).
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402  (must come after choosing the backend)
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator  # noqa: E402

# Same palette and chrome as plot_phase2.py (dataviz reference palette).
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BOOK_COLOURS = {"ReferenceBook": "#2a78d6", "FastBook": "#eb6834", "TickFastBook": "#1baf7a"}
BOOKS = list(BOOK_COLOURS)  # fixed order: the colour follows the book, never its rank

PERCENTILES = [("p50", 0.5), ("p90", 0.9), ("p99", 0.99), ("p99.9", 0.999), ("p99.99", 0.9999)]
CSV_COLUMN = {"p50": "p50_ns", "p90": "p90_ns", "p99": "p99_ns", "p99.9": "p999_ns",
              "p99.99": "p9999_ns"}
BINS_PER_DECADE = 10

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


# ---------------------------------------------------------------------------------------------
# Reading the CSV files
# ---------------------------------------------------------------------------------------------

def read_csv(path: Path) -> tuple[dict, list[dict]]:
    """The '# key: value' comment lines as a dict (plus 'banner' if present), and the rows."""
    env, lines = {}, []
    with open(path, newline="", encoding="utf-8") as f:
        for line in f:
            if line.startswith("# "):
                text = line[2:].strip()
                if text.startswith("***"):
                    env["banner"] = text.strip("* ").strip()
                elif ": " in text:
                    key, value = text.split(": ", 1)
                    env[key] = value
            else:
                lines.append(line)
    return env, list(csv.DictReader(lines))


def histogram_path(csv_path: Path) -> Path:
    """FILE.csv -> FILE_histogram.csv, the same rule as nanobook_bench."""
    stem = csv_path.name[:-4] if csv_path.name.endswith(".csv") else csv_path.name
    return csv_path.with_name(stem + "_histogram.csv")


def headline_rows(rows: list[dict]) -> dict:
    """{(scenario, book): row}: the 'all' row of a mixed run, or a single-kind run's one tag."""
    by_run = defaultdict(list)
    for r in rows:
        if r["mode"] == "latency":
            by_run[(r["scenario"], r["book"])].append(r)
    picked = {}
    for key, rs in by_run.items():
        all_rows = [r for r in rs if r["tag"] == "all"]
        picked[key] = all_rows[0] if all_rows else rs[0]
    return picked


# ---------------------------------------------------------------------------------------------
# Chart helpers
# ---------------------------------------------------------------------------------------------

def fmt_ns(v: float, _pos=None) -> str:
    if v >= 1_000_000:
        return f"{v / 1_000_000:g} ms"
    if v >= 1_000:
        return f"{v / 1_000:g} µs"
    return f"{v:g} ns"


def nines(q: float) -> float:
    """p90 -> 1, p99 -> 2, p99.9 -> 3: each step is ten times rarer."""
    return -math.log10(1 - q)


def decades(low: float, high: float) -> list[float]:
    """1, 10, 100, ... covering [low, high]."""
    start = math.floor(math.log10(max(low, 1)))
    stop = math.ceil(math.log10(max(high, 1)))
    return [10.0 ** k for k in range(start, stop + 1)]


def tidy(ax) -> None:
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)


HEADER_IN = 1.0  # inches above the panels: title, banner, legend
FOOTER_IN = 0.3  # inches below them: the environment


def from_top(fig, inches: float) -> float:
    """A position `inches` below the top of the figure, as a fraction of its height, so the
    header looks the same however many rows of panels there are."""
    return 1 - inches / fig.get_size_inches()[1]


def legend_strip(fig, books) -> None:
    handles = [plt.Line2D([0], [0], color=BOOK_COLOURS[b], linewidth=2) for b in books]
    fig.legend(handles, books, loc="upper center", ncol=len(books), frameon=False,
               bbox_to_anchor=(0.5, from_top(fig, 0.62)), fontsize=8.5, labelcolor=INK_2)


def frame(fig, env: dict, title: str) -> None:
    """Title, the NOT FOR PUBLISHING banner if any, and the environment as a footer (rule 19)."""
    fig.suptitle(title, x=0.01, y=from_top(fig, 0.05), ha="left", va="top", fontsize=11,
                 fontweight="bold", color=INK)
    if "banner" in env:
        fig.text(0.01, from_top(fig, 0.36), "*** " + env["banner"] + " ***", ha="left",
                 va="top", fontsize=9, fontweight="bold", color=INK)
    footer = " · ".join(env.get(k, "?") for k in ("cpu", "kernel", "compiler", "flags"))
    footer += (f" · core {env.get('pinned to core', '?')} · isolated {env.get('isolated cores', '?')}"
               f" · commit {env.get('commit', '?')} · seed {env.get('seed', '?')}")
    fig.text(0.01, 0.005, footer, ha="left", va="bottom", fontsize=6.5, color=MUTED)


def panel_title(scenario: str, tag: str) -> str:
    """'sweep10 (match)', 'mixed (all)', but plain 'add' rather than 'add (add)'."""
    return scenario if tag == scenario else f"{scenario} ({tag})"


def grid_of(n: int, cols: int = 3):
    rows = math.ceil(n / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(10, 2.55 * rows + HEADER_IN + FOOTER_IN),
                             squeeze=False)
    return fig, axes.flatten()


def finish(fig, out: Path) -> None:
    height = fig.get_size_inches()[1]
    fig.tight_layout(rect=(0, FOOTER_IN / height, 1, from_top(fig, HEADER_IN)))
    fig.savefig(out, dpi=200)
    plt.close(fig)


# ---------------------------------------------------------------------------------------------
# The two charts
# ---------------------------------------------------------------------------------------------

def plot_percentiles(env: dict, rows: list[dict], out: Path) -> None:
    picked = headline_rows(rows)
    scenarios = list(dict.fromkeys(s for s, _ in picked))
    books = [b for b in BOOKS if any(b == bk for _, bk in picked)]
    fig, axes = grid_of(len(scenarios))
    for ax, scenario in zip(axes, scenarios):
        lo, hi = math.inf, 0.0
        shown = set()  # the percentiles any book has (p99.99 needs 1,000,000+ samples)
        for book in books:
            row = picked.get((scenario, book))
            if row is None:
                continue
            points = [(nines(q), float(row[CSV_COLUMN[name]])) for name, q in PERCENTILES
                      if row[CSV_COLUMN[name]] != ""]  # p99.99 is empty below 1,000,000 samples
            xs, ys = zip(*points)
            shown.update(xs)
            ys = [max(y, 1.0) for y in ys]  # a 0 ns sample (overhead subtracted) can't go on a log axis
            ax.plot(xs, ys, color=BOOK_COLOURS[book], linewidth=2, marker="o", markersize=3.5,
                    markeredgecolor=SURFACE, markeredgewidth=1)
            lo, hi = min(lo, min(ys)), max(hi, max(ys))
        ax.set_yscale("log")
        ticks = decades(lo, hi)
        ax.yaxis.set_major_locator(FixedLocator(ticks))
        ax.yaxis.set_minor_locator(NullLocator())
        ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_ylim(ticks[0], ticks[-1])
        names = [(nines(q), n) for n, q in PERCENTILES if nines(q) in shown]
        ax.set_xticks([x for x, _ in names], [n for _, n in names])
        ax.set_xlim(names[0][0] - 0.2, names[-1][0] + 0.2)
        ax.set_title(panel_title(scenario, picked[(scenario, books[0])]["tag"]), loc="left")
        tidy(ax)
    for ax in axes[len(scenarios):]:
        ax.axis("off")
    legend_strip(fig, books)
    frame(fig, env, "Latency per operation at each percentile (lower is better)")
    finish(fig, out)


def plot_histograms(env: dict, rows: list[dict], headline: dict, out: Path) -> None:
    # Regroup the harness's buckets into equal-width bins on a log axis: bin k covers
    # [10^(k/10), 10^((k+1)/10)) ns. A bucket goes to the bin of its middle.
    counts = defaultdict(lambda: defaultdict(int))  # (scenario, book) -> {bin: count}
    for r in rows:
        key = (r["scenario"], r["book"])
        if key not in headline or r["tag"] != headline[key]["tag"]:
            continue
        mid = max((int(r["low_ns"]) + int(r["high_ns"])) / 2, 1.0)
        counts[key][math.floor(math.log10(mid) * BINS_PER_DECADE)] += int(r["count"])

    scenarios = list(dict.fromkeys(s for s, _ in headline))
    books = [b for b in BOOKS if any(b == bk for _, bk in headline)]
    fig, axes = grid_of(len(scenarios))
    for ax, scenario in zip(axes, scenarios):
        lo, hi = math.inf, 0.0
        for book in books:
            bins = counts.get((scenario, book))
            if not bins:
                continue
            total = sum(bins.values())
            first, last = min(bins), max(bins)
            edges = [10 ** (k / BINS_PER_DECADE) for k in range(first, last + 2)]
            shares = [100 * bins.get(k, 0) / total for k in range(first, last + 1)]
            ax.stairs(shares, edges, color=BOOK_COLOURS[book], linewidth=1.6)
            lo, hi = min(lo, edges[0]), max(hi, edges[-1])
        ax.set_xscale("log")
        ax.set_yscale("log")
        ticks = decades(lo, hi)
        ax.xaxis.set_major_locator(FixedLocator(ticks))
        ax.xaxis.set_minor_locator(NullLocator())
        ax.xaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_xlim(lo, hi)
        ax.yaxis.set_major_locator(FixedLocator([0.001, 0.01, 0.1, 1, 10, 100]))
        ax.yaxis.set_minor_locator(NullLocator())
        ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
        ax.set_ylim(0.0005, 100)
        ax.set_title(panel_title(scenario, headline[(scenario, books[0])]["tag"]), loc="left")
        tidy(ax)
    for ax in axes[len(scenarios):]:
        ax.axis("off")
    legend_strip(fig, books)
    frame(fig, env, "Share of operations by latency (log scales: the long right tail is the slow ones)")
    finish(fig, out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("csv", type=Path, help="the summary CSV written by nanobook_bench --csv")
    parser.add_argument("--out-dir", type=Path, help="where to write the PNGs (default: the CSV's folder)")
    parser.add_argument("--prefix", default="latency", help="file name prefix (default: latency)")
    args = parser.parse_args()

    env, rows = read_csv(args.csv)
    _, hist_rows = read_csv(histogram_path(args.csv))
    out_dir = args.out_dir or args.csv.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    headline = headline_rows(rows)
    if not headline:
        raise SystemExit(f"{args.csv}: no latency rows (a --throughput CSV has nothing to plot)")
    plot_percentiles(env, rows, out_dir / f"{args.prefix}_percentiles.png")
    plot_histograms(env, hist_rows, headline, out_dir / f"{args.prefix}_histograms.png")
    print(f"wrote {args.prefix}_percentiles.png and {args.prefix}_histograms.png in {out_dir}")


if __name__ == "__main__":
    main()

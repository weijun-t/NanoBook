"""plot_phase4.py: draws the phase 4 report's charts from the recorded real-day numbers.

What it does: Reads CSV files in docs/data/ and writes two PNG charts to docs/img/:
  phase4_latency.png  latency at each percentile, one panel per ITCH message kind (and all of
                      them), the real day against phase 3's single book where an operation
                      matches
  phase4_flow.png     what real flow does to the sorted price levels: how many levels each new or
                      vanishing level shifts, and how far new orders land from the best price
  Usage: .venv/bin/python scripts/plot_phase4.py [--data DIR] [--out DIR]
  (defaults: docs/data and docs/img).

Why it exists: plan §7.5: the plots read only CSV and never touch the engine, so they can be
  redrawn at any time from the numbers recorded in docs/perf-log.md and docs/itch-design.md §9.1.

How it works:
  - Latency: phase4_wsl_round1/2.csv (nanobook_replay --latency) and phase3_wsl_round1/2.csv
    (nanobook_bench), each averaged over its two rounds, as the perf log's tables are. x is
    spaced by "number of nines", as in plot_latency.py. Phase 3 pairs: add -> add, delete -> cancel,
    partial cancel -> modify, all -> mixed. Execute and replace have no phase 3 equivalent.
  - Flow: phase4_flow.csv (nanobook_replay --flow --csv). Shifts on a log scale, because the
    rare deep shifts are the point; distances as one bar each, labelled with their share.
  - Colours (dataviz reference palette): the real day in FastBook's orange (slot 2), as in the
    phase 2 and 3 charts; phase 3 as a grey dashed reference; created/removed levels in slots 1
    and 3 (blue, aqua), a pair the palette validates for colour-blind readers.

Trade-offs: Static PNGs for the report, light mode only. Needs matplotlib (requirements.txt).
"""

import argparse
import csv
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402  (must come after choosing the backend)
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
REAL_DAY = "#eb6834"  # slot 2: FastBook's colour in every NanoBook chart
PHASE3 = "#898781"    # a reference, not a competing series: grey, dashed
CREATED = "#2a78d6"   # slot 1
REMOVED = "#1baf7a"   # slot 3

PERCENTILES = [("p50", 0.5), ("p90", 0.9), ("p99", 0.99), ("p99.9", 0.999), ("p99.99", 0.9999)]
CSV_COLUMN = {"p50": "p50_ns", "p90": "p90_ns", "p99": "p99_ns", "p99.9": "p999_ns",
              "p99.99": "p9999_ns"}
# (panel title, phase 4 tag, phase 3 (scenario, tag) or None)
PANELS = [
    ("add (A, F)", "add", ("add", "add")),
    ("delete (D)", "cancel", ("cancel", "cancel")),
    ("partial cancel (X)", "modify", ("modify", "modify")),
    ("execute (E, C)", "match", None),
    ("replace (U)", "replace", None),
    ("all order messages", "all", ("mixed", "all")),
]

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


def read_csv(path: Path) -> tuple[dict, list[dict]]:
    """The '# key: value' comment lines as a dict (plus 'banner'), and the rows."""
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


def averaged(paths: list[Path], scenario: str, book: str, tag: str) -> dict:
    """{percentile name: ns}, the mean over the rounds, for one latency row. p99.99 only where
    1,000,000+ samples back it (phase 3's rule), and a percentile only if every round has it."""
    rows = []
    for path in paths:
        _, all_rows = read_csv(path)
        rows += [r for r in all_rows if r["mode"] == "latency" and r["scenario"] == scenario
                 and r["book"] == book and r["tag"] == tag]
    values = {}
    for name, _ in PERCENTILES:
        found = [float(r[CSV_COLUMN[name]]) for r in rows if r[CSV_COLUMN[name]] != ""
                 and (name != "p99.99" or int(r["count"]) >= 1_000_000)]
        if rows and len(found) == len(paths):
            values[name] = sum(found) / len(found)
    return values


def nines(q: float) -> float:
    return -math.log10(1 - q)


def fmt_ns(v: float, _pos=None) -> str:
    if v >= 1_000:
        return f"{v / 1_000:g} µs"
    return f"{v:g} ns"


def tidy(ax) -> None:
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)


def footer(fig, env: dict) -> None:
    text = " · ".join(env.get(k, "?") for k in ("cpu", "kernel", "compiler", "flags"))
    text += f" · core {env.get('pinned to core', '?')} · commit {env.get('commit', '?')}"
    fig.text(0.01, 0.005, text, ha="left", va="bottom", fontsize=6.5, color=MUTED)


def plot_latency(data: Path, out: Path) -> None:
    p4 = [data / "phase4_wsl_round1.csv", data / "phase4_wsl_round2.csv"]
    p3 = [data / "phase3_wsl_round1.csv", data / "phase3_wsl_round2.csv"]
    env, _ = read_csv(p4[0])
    fig, axes = plt.subplots(2, 3, figsize=(10, 6.4))
    for ax, (title, tag, phase3) in zip(axes.flatten(), PANELS):
        curves = [(averaged(p4, "itch_day", "FastBook", tag), REAL_DAY, "-")]
        if phase3:
            curves.append((averaged(p3, phase3[0], "FastBook", phase3[1]), PHASE3, "--"))
        for values, colour, style in curves:
            xs = [nines(q) for name, q in PERCENTILES if name in values]
            ys = [values[name] for name, _ in PERCENTILES if name in values]
            ax.plot(xs, ys, color=colour, linestyle=style, linewidth=2, marker="o",
                    markersize=4, markeredgecolor=SURFACE, markeredgewidth=1)
        caption = f"p50: {curves[0][0]['p50']:.0f} ns real day"
        if phase3:
            caption += f" · {curves[1][0]['p50']:.0f} ns phase 3"
        ax.text(0.0, 1.0, caption, transform=ax.transAxes, va="bottom", fontsize=7.5,
                color=INK_2)
        ax.set_yscale("log")
        ticks = [10, 100, 1_000, 10_000]
        ax.yaxis.set_major_locator(FixedLocator(ticks))
        ax.yaxis.set_minor_locator(NullLocator())
        ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_ylim(8, 30_000)
        ax.set_xticks([nines(q) for _, q in PERCENTILES], [n for n, _ in PERCENTILES])
        ax.set_xlim(nines(0.5) - 0.2, nines(0.9999) + 0.2)
        ax.set_title(title if phase3 else f"{title}: no phase 3 equivalent", loc="left", pad=14)
        tidy(ax)
    handles = [plt.Line2D([0], [0], color=REAL_DAY, linewidth=2),
               plt.Line2D([0], [0], color=PHASE3, linewidth=2, linestyle="--")]
    fig.legend(handles, ["real Nasdaq day: 8,713 books, 30 Jan 2019",
                         "phase 3: one book, made-up orders, in cache"],
               loc="upper left", ncol=2, frameon=False, bbox_to_anchor=(0.003, 0.905),
               fontsize=8.5, labelcolor=INK_2)
    fig.suptitle("FastBook latency per order message (lower is better; average of two rounds)",
                 x=0.01, y=0.985, ha="left", fontsize=11, fontweight="bold", color=INK)
    if "banner" in env:
        fig.text(0.01, 0.935, "*** " + env["banner"] + " ***", ha="left", fontsize=9,
                 fontweight="bold", color=INK)
    footer(fig, env)
    fig.tight_layout(rect=(0, 0.03, 1, 0.88))
    fig.savefig(out, dpi=200)
    plt.close(fig)


def plot_flow(data: Path, out: Path) -> None:
    info, rows = read_csv(data / "phase4_flow.csv")
    tables = {}
    for r in rows:
        tables.setdefault(r["table"], []).append(r)
    fig, (left, right) = plt.subplots(1, 2, figsize=(10, 4.4), gridspec_kw={"width_ratios": [3, 2]})

    # Left: levels shifted per level created / removed, as a share of each, log scale.
    created, removed = tables["levels_shifted_by_creation"], tables["levels_shifted_by_removal"]
    last = max(i for i, r in enumerate(created) if int(r["count"]) or int(removed[i]["count"]))
    labels = [r["low"] if r["low"] == r["high"] else f"{r['low']}–{r['high']}"
              for r in created[: last + 1]]
    width = 0.4
    for offset, rs, colour in ((-width / 2, created, CREATED), (width / 2, removed, REMOVED)):
        total = sum(int(r["count"]) for r in rs)
        shares = [100 * int(r["count"]) / total for r in rs[: last + 1]]
        left.bar([i + offset for i in range(last + 1)], shares, width=width - 0.04, color=colour,
                 linewidth=0)
    deep = sum(int(r["count"]) for r in created if int(r["low"]) >= 1024)
    if deep:
        created_total = sum(int(r["count"]) for r in created)
        tail = 100 * int(created[last]["count"]) / created_total
        left.annotate(f"{deep:,} creations shift 1,024+ levels\n"
                      "(phase 3's deep_insert did, every time)", (last - width / 2, tail),
                      xytext=(0.58, 0.86), textcoords="axes fraction", fontsize=7.5, color=INK_2,
                      arrowprops={"arrowstyle": "-", "color": MUTED, "linewidth": 0.8})
    left.set_yscale("log")
    left.yaxis.set_major_locator(FixedLocator([0.001, 0.01, 0.1, 1, 10, 100]))
    left.yaxis.set_minor_locator(NullLocator())
    left.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
    left.set_ylim(0.001, 100)
    left.set_xticks(range(last + 1), labels, rotation=45, ha="right")
    left.set_xlabel("levels shifted in the sorted array")
    left.set_title("Each new or vanishing price level: levels it shifts", loc="left")
    fig.legend([plt.Rectangle((0, 0), 1, 1, color=CREATED), plt.Rectangle((0, 0), 1, 1, color=REMOVED)],
               ["level created", "level removed"], loc="upper left", ncol=2, frameon=False,
               bbox_to_anchor=(0.003, 0.925), fontsize=8.5, labelcolor=INK_2)
    tidy(left)

    # Right: distance of each new order from its side's best price.
    names = ["side was empty", "improves the best", "at the best", "behind < $0.01", "< $0.10",
             "< $1", "< $10", "< $100", "$100 or more"]
    counts = [int(r["count"]) for r in tables["distance_from_best"]]
    total = sum(counts)
    shares = [100 * c / total for c in counts]
    ys = list(range(len(names)))[::-1]
    right.barh(ys, shares, color=CREATED, height=0.7, linewidth=0)
    for y, share in zip(ys, shares):
        right.text(share + 1, y, f"{share:.2f}%" if share < 1 else f"{share:.1f}%", va="center",
                   fontsize=7.5, color=INK_2)
    right.set_yticks(ys, [n.replace("$", "\\$") for n in names])
    right.set_xlim(0, max(shares) * 1.25)
    right.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
    right.set_title("Each new order: distance from the best", loc="left")
    for side in ("top", "right"):
        right.spines[side].set_visible(False)
    right.spines["left"].set_color(BASELINE)
    right.xaxis.grid(True, color=GRID, linewidth=0.6)
    right.set_axisbelow(True)
    right.tick_params(length=0)

    fig.suptitle(f"Real order flow: {sum(int(r['count']) for r in created):,} price levels "
                 "created and removed over the day", x=0.01, y=0.985, ha="left", fontsize=11,
                 fontweight="bold", color=INK)
    fig.text(0.01, 0.005, f"input {info.get('input', '?')} · commit {info.get('commit', '?')} · "
             "nanobook_replay --flow (untimed)", ha="left", va="bottom", fontsize=6.5, color=MUTED)
    fig.tight_layout(rect=(0, 0.03, 1, 0.88))
    fig.savefig(out, dpi=200)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--data", type=Path, default=ROOT / "docs" / "data")
    parser.add_argument("--out", type=Path, default=ROOT / "docs" / "img")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    plot_latency(args.data, args.out / "phase4_latency.png")
    plot_flow(args.data, args.out / "phase4_flow.png")
    print(f"wrote phase4_latency.png and phase4_flow.png in {args.out}")


if __name__ == "__main__":
    main()

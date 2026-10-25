"""plot_compare.py: draws two sets of latency CSVs on one chart, "before" against "after".

What it does: Reads CSV files written by nanobook_replay --latency or nanobook_bench --csv (one
  file per round) for a "before" and an "after" build, and writes one PNG: latency at each
  percentile, one panel per message kind, every round drawn as its own line.
  Usage: .venv/bin/python scripts/plot_compare.py --before A1.csv A2.csv --after B1.csv B2.csv
           --out FILE.png [--labels BEFORE AFTER] [--book FastBook] [--title TEXT]

Why it exists: plan §7.5 promises a before/after chart, and phase 7 uses it once, for the whole
  of phase 6 in one picture (polish design §6.2). A table gives the numbers; the chart shows
  at a glance where on the curve a change helped, and whether the rounds agree.

How it works:
  - Panels: one per tag (add, cancel, ...) in the order the first "before" file lists them,
    keeping only the chosen book's latency rows. For a replay CSV the tags get their ITCH
    names; a nanobook_bench CSV, which has one scenario per row, is labelled by scenario.
  - x is spaced by "number of nines" (p90 = 1, p99 = 2, p99.9 = 3, p99.99 = 4), as in
    plot_latency.py, so each step right is ten times rarer. y is log scale.
  - Each round is a thin line, so the noise is visible: if the rounds of one build overlap
    the other's, the difference is noise. Each panel's caption gives the p50s, averaged over
    the rounds, and the change in percent.
  - The legend gives each build's throughput (ns per message, the throughput row's mean,
    averaged over rounds) when the CSVs have one. The footer gives the machine and both
    commits (rule 19), and the chart repeats a NOT FOR PUBLISHING banner from any input.

Trade-offs: Static PNG, light mode only. Colours: "before" in grey (the old state, a
  reference), "after" in FastBook's orange (slot 2 of the dataviz palette), as in the other
  NanoBook charts. Needs matplotlib (requirements.txt).
"""

import argparse
import csv
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402  (must come after choosing the backend)
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BEFORE = "#898781"  # the old state is a reference: grey
AFTER = "#eb6834"   # slot 2: FastBook's colour in every NanoBook chart

PERCENTILES = [("p50", 0.5), ("p90", 0.9), ("p99", 0.99), ("p99.9", 0.999), ("p99.99", 0.9999)]
CSV_COLUMN = {"p50": "p50_ns", "p90": "p90_ns", "p99": "p99_ns", "p99.9": "p999_ns",
              "p99.99": "p9999_ns"}
TICKS = [m * 10 ** k for k in range(1, 6) for m in (1, 2, 5)]  # 10, 20, 50, 100, ... ns
ITCH_NAMES = {"add": "add (A, F)", "cancel": "delete (D)", "modify": "partial cancel (X)",
              "match": "execute (E, C)", "replace": "replace (U)", "all": "all order messages"}

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


def panel_key(row: dict) -> str:
    """A replay CSV has one scenario (itch_day) and many tags; a bench CSV one per scenario."""
    if row["scenario"] == "itch_day":
        return ITCH_NAMES.get(row["tag"], row["tag"])
    return row["scenario"]


def latency_rows(rows: list[dict], book: str) -> dict:
    """{panel key: {percentile name: ns}} for the book's latency rows."""
    out = {}
    for r in rows:
        if r["mode"] != "latency" or r["book"] != book:
            continue
        if r["scenario"] != "itch_day" and r["tag"] not in (r["scenario"], "all"):
            continue  # a bench "mixed" run: keep only its "all" row
        out[panel_key(r)] = {name: float(r[col]) for name, col in CSV_COLUMN.items()
                             if r[col] not in ("", "-")}
    return out


def throughput_ns(rows: list[dict], book: str) -> float | None:
    for r in rows:
        if r["mode"] == "throughput" and r["book"] == book and r["mean_ns"]:
            return float(r["mean_ns"])
    return None


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


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--before", type=Path, nargs="+", required=True)
    parser.add_argument("--after", type=Path, nargs="+", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--labels", nargs=2, default=["before", "after"])
    parser.add_argument("--book", default="FastBook")
    parser.add_argument("--title", default="Latency before and after (lower is better)")
    args = parser.parse_args()

    builds = []  # (label, colour, [ {panel: values} per round ], [throughput per round], env)
    for label, colour, paths in ((args.labels[0], BEFORE, args.before),
                                 (args.labels[1], AFTER, args.after)):
        rounds, speeds, env = [], [], {}
        for path in paths:
            env, rows = read_csv(path)
            rounds.append(latency_rows(rows, args.book))
            speed = throughput_ns(rows, args.book)
            if speed is not None:
                speeds.append(speed)
        builds.append((label, colour, rounds, speeds, env))

    panels = list(builds[0][2][0])
    if not panels:
        raise SystemExit(f"no latency rows for book {args.book} in {args.before[0]}")
    columns = min(3, len(panels))
    rows_n = math.ceil(len(panels) / columns)
    fig, axes = plt.subplots(rows_n, columns, figsize=(3.4 * columns, 2.8 * rows_n + 1.2),
                             squeeze=False)
    for ax, key in zip(axes.flatten(), panels):
        p50s = []
        for _, colour, rounds, _, _ in builds:
            for values in rounds:
                values = values.get(key, {})
                xs = [nines(q) for name, q in PERCENTILES if name in values]
                ys = [values[name] for name, _ in PERCENTILES if name in values]
                ax.plot(xs, ys, color=colour, linewidth=1.4, marker="o", markersize=3,
                        markeredgecolor=SURFACE, markeredgewidth=0.8)
            p50s.append(mean([r[key]["p50"] for r in rounds if key in r]))
        change = (p50s[1] - p50s[0]) / p50s[0] * 100
        ax.text(0.0, 1.0, f"p50: {p50s[0]:.0f} → {p50s[1]:.0f} ns ({change:+.0f}%)",
                transform=ax.transAxes, va="bottom", fontsize=7.5, color=INK_2)
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(FixedLocator(TICKS))
        ax.yaxis.set_minor_locator(NullLocator())
        ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_xticks([nines(q) for _, q in PERCENTILES], [n for n, _ in PERCENTILES])
        ax.set_xlim(nines(0.5) - 0.2, nines(0.9999) + 0.2)
        ax.set_title(key, loc="left", pad=14)
        tidy(ax)
    for ax in axes.flatten()[len(panels):]:
        ax.set_visible(False)

    # The header, top down, in inches: title, the banner (if any), the legend.
    height = fig.get_figheight()
    env_before, env_after = builds[0][4], builds[1][4]
    banner = env_before.get("banner") or env_after.get("banner")
    fig.suptitle(args.title, x=0.01, y=1 - 0.12 / height, ha="left", va="top", fontsize=11,
                 fontweight="bold", color=INK)
    top = 0.45
    if banner:
        fig.text(0.01, 1 - top / height, "*** " + banner + " ***", ha="left", va="top",
                 fontsize=8, fontweight="bold", color=INK)
        top += 0.25
    handles, names = [], []
    for label, colour, rounds, speeds, _ in builds:
        handles.append(plt.Line2D([0], [0], color=colour, linewidth=2))
        name = f"{label} ({len(rounds)} rounds)"
        if speeds:
            name += f": {mean(speeds):.1f} ns per message"
        names.append(name)
    fig.legend(handles, names, loc="upper left", ncol=2, frameon=False,
               bbox_to_anchor=(0.003, 1 - (top - 0.05) / height), fontsize=8.5,
               labelcolor=INK_2)
    top += 0.35
    footer = " · ".join(env_after.get(k, "?") for k in ("cpu", "kernel", "compiler", "flags"))
    footer += (f" · isolated cores {env_after.get('isolated cores', '?')}"
               f" · commits {env_before.get('commit', '?')} → {env_after.get('commit', '?')}")
    fig.text(0.01, 0.005, footer, ha="left", va="bottom", fontsize=6.5, color=MUTED)
    fig.tight_layout(rect=(0, 0.03, 1, 1 - top / height))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=200)
    plt.close(fig)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()

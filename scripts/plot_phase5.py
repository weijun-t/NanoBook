"""plot_phase5.py: draws the phase 5 report's charts from the recorded pipeline session.

What it does: Reads the files scripts/run_pipeline_session.sh wrote and writes two PNG charts:
  phase5_latency.png     end-to-end latency (min, p50, p90, p99) against the feed rate, with the
                         rate the pipeline can sustain marked
  phase5_throughput.png  messages per second: the whole day on one thread and on three, and the
                         rate the three threads sustained when overloaded at 8 M records/s
  Usage: .venv/bin/python scripts/plot_phase5.py [--data DIR] [--out DIR]
  (defaults: docs/data/phase5_wsl and docs/img).

Why it exists: plan §7.5: the plots read only recorded files and never touch the engine, so
  they can be redrawn at any time from the numbers in docs/perf-log.md.

How it works:
  - Latency: latency_<rate>_round1/2.csv (nanobook_pipeline --latency), each percentile averaged
    over the two rounds, as the perf log's tables are. Both axes on log scales: rates double from
    0.5 to 8 M records/s, and latency runs from 170 ns to half a second.
  - The sustained rate: at 8 M/s the last update arrives `max` late, so the pipeline handled
    count records in count / rate + max seconds (perf log, "What these numbers say").
  - Throughput: throughput_round1/2.txt (nanobook_pipeline --throughput), parsed from the lines
    "one thread: ... ns per message" and "three threads: ..."; bars are the average, dots each
    round, so the round-to-round noise shows.
  - Colours (dataviz reference palette): the four percentiles are ordered, so one hue, light to
    dark (blue steps 250, 400, 550, 700; ordinal ramps from step 250 clear 2:1 on the light
    surface), and every line is labelled at its left end, so colour is never the only key.
    The four lines are close there, so the labels are spread: each at least 1.7x above the one
    below (log axis), the group centred on the lines.

Trade-offs: Static PNGs for the report, light mode only. Needs matplotlib (requirements.txt).
"""

import argparse
import csv
import math
import re
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
BAR = "#2a78d6"  # slot 1: a single series, so the first categorical hue
# (label, CSV column, colour): ordered, so one hue from light to dark
PERCENTILES = [("min", "min_ns", "#86b6ef"), ("p50", "p50_ns", "#3987e5"),
               ("p90", "p90_ns", "#1c5cab"), ("p99", "p99_ns", "#0d366b")]
RATES = [500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000]
ROUNDS = (1, 2)

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


def latency_row(data: Path, rate: int, rnd: int) -> dict:
    _, rows = read_csv(data / f"latency_{rate}_round{rnd}.csv")
    return next(r for r in rows if r["mode"] == "latency" and r["tag"] == "all")


def sustained_rate(row: dict, rate: int) -> float:
    """Records per second an overloaded run kept up with: count / (count / rate + max)."""
    count = int(row["count"])
    return count / (count / rate + float(row["max_ns"]) / 1e9)


def throughput(data: Path, rnd: int) -> dict:
    """{'one thread': ns per message, 'three threads': ns per message} from one round's text."""
    text = (data / f"throughput_round{rnd}.txt").read_text(encoding="utf-8")
    found = re.findall(r"(one thread|three threads):\s+[\d.]+ s, ([\d.]+) ns per message", text)
    return {name: float(ns) for name, ns in found}


def fmt_ns(v: float, _pos=None) -> str:
    if v >= 1e9:
        return f"{v / 1e9:g} s"
    if v >= 1e6:
        return f"{v / 1e6:g} ms"
    if v >= 1e3:
        return f"{v / 1e3:g} µs"
    return f"{v:g} ns"


def spread(ys: list[float], ratio: float) -> list[float]:
    """Label heights on a log axis: each at least `ratio` above the one below, centred on ys."""
    order = sorted(range(len(ys)), key=lambda i: ys[i])
    logs = [math.log(ys[i]) for i in order]
    placed = [logs[0]]
    for value in logs[1:]:
        placed.append(max(value, placed[-1] + math.log(ratio)))
    shift = (sum(logs) - sum(placed)) / len(logs)  # keep the group's middle on the lines'
    out = [0.0] * len(ys)
    for i, value in zip(order, placed):
        out[i] = math.exp(value + shift)
    return out


def tidy(ax, grid_axis: str = "y") -> None:
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    getattr(ax, f"{grid_axis}axis").grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)


def from_top(fig, inches: float) -> float:
    """A figure y position `inches` below the top, whatever the figure's height."""
    return 1 - inches / fig.get_figheight()


def frame(fig, env: dict, title: str) -> None:
    fig.text(0.01, from_top(fig, 0.1), title, ha="left", va="top", fontsize=11,
             fontweight="bold", color=INK)
    if "banner" in env:
        fig.text(0.01, from_top(fig, 0.36), "*** " + env["banner"] + " ***", ha="left",
                 va="top", fontsize=9, fontweight="bold", color=INK)
    text = " · ".join(env.get(k, "?") for k in ("cpu", "kernel", "compiler", "flags"))
    text += f" · cores {env.get('cores', '?')} · commit {env.get('commit', '?')}"
    fig.text(0.01, 0.005, text, ha="left", va="bottom", fontsize=6.5, color=MUTED)


def plot_latency(data: Path, out: Path) -> float:
    env, _ = read_csv(data / f"latency_{RATES[0]}_round1.csv")
    rows = {rate: [latency_row(data, rate, rnd) for rnd in ROUNDS] for rate in RATES}
    xs = [rate / 1e6 for rate in RATES]
    fig, ax = plt.subplots(figsize=(8, 4.6))
    lines = []
    for label, column, colour in PERCENTILES:
        ys = [sum(float(r[column]) for r in rows[rate]) / len(ROUNDS) for rate in RATES]
        ax.plot(xs, ys, color=colour, linewidth=2, marker="o", markersize=5,
                markeredgecolor=SURFACE, markeredgewidth=1)
        lines.append((label, ys[0]))
    for label, y in zip([name for name, _ in lines], spread([y for _, y in lines], 1.7)):
        ax.text(xs[0] / 1.1, y, label, ha="right", va="center", fontsize=8, color=INK_2)
    capacity = sum(sustained_rate(r, RATES[-1]) for r in rows[RATES[-1]]) / len(ROUNDS)
    ax.axvline(capacity / 1e6, color=MUTED, linewidth=1, linestyle="--")
    ax.axvspan(capacity / 1e6, xs[-1] * 1.3, color=GRID, alpha=0.45, linewidth=0)
    ax.text(capacity / 1e6 * 1.04, 1e8, f"{capacity / 1e6:.1f} M/s\nsustained", fontsize=7.5,
            color=INK_2, va="center")
    ax.text(xs[-1] / 1.06, 150, "overloaded:\nlatency is\nthe backlog", fontsize=7.5,
            color=INK_2, ha="right", va="bottom")
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(xs))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g} M"))
    ax.set_xlim(xs[0] / 1.6, xs[-1] * 1.3)  # room on the left for the line labels
    ax.set_xlabel("feed rate (records per second)")
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(FixedLocator([1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9]))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ax.set_ylim(100, 2e9)
    ax.set_title("From each record's intended send time (feed core) to its update on the "
                 "publisher core.\n10:00–10:05 (5,641,949 records), every update timed, "
                 "average of two rounds.", loc="left", fontsize=8, fontweight="normal",
                 color=INK_2, pad=8)
    tidy(ax)
    frame(fig, env, "Pipeline latency against feed rate (lower is better)")
    fig.tight_layout(rect=(0, 0.03, 1, from_top(fig, 0.6)))
    fig.savefig(out, dpi=200)
    plt.close(fig)
    return capacity


def plot_throughput(data: Path, out: Path, capacity: float) -> None:
    env, _ = read_csv(data / f"latency_{RATES[-1]}_round1.csv")
    rounds = [throughput(data, rnd) for rnd in ROUNDS]
    overload = [sustained_rate(latency_row(data, RATES[-1], rnd), RATES[-1]) / 1e6
                for rnd in ROUNDS]
    bars = [("one thread, whole day", [1e3 / r["one thread"] for r in rounds]),
            ("three threads, whole day", [1e3 / r["three threads"] for r in rounds]),
            ("three threads, overloaded at 8 M/s\n(10:00–10:05)", overload)]
    fig, ax = plt.subplots(figsize=(8, 3.2))
    ys = list(range(len(bars)))[::-1]
    for y, (_, values) in zip(ys, bars):
        mean = sum(values) / len(values)
        ax.barh(y, mean, height=0.55, color=BAR, linewidth=0)
        ax.plot(values, [y] * len(values), "o", color=INK, markersize=3.5)
        ax.text(max(values) + 0.1, y, f"{mean:.1f} M/s", va="center", fontsize=8, color=INK_2)
    ax.set_yticks(ys, [name for name, _ in bars])
    ax.tick_params(axis="y", labelcolor=INK_2)
    ax.set_xlim(0, 7)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g} M"))
    ax.set_xlabel("messages per second (bar: average; dots: each round)")
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.spines["left"].set_color(BASELINE)
    ax.spines["bottom"].set_visible(False)
    ax.xaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)
    frame(fig, env, "Throughput: three threads are slower than one (higher is better)")
    fig.tight_layout(rect=(0, 0.04, 1, from_top(fig, 0.6)))
    fig.savefig(out, dpi=200)
    plt.close(fig)
    print(f"sustained at 8 M/s: {capacity / 1e6:.2f} M records/s")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--data", type=Path, default=ROOT / "docs" / "data" / "phase5_wsl")
    parser.add_argument("--out", type=Path, default=ROOT / "docs" / "img")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    capacity = plot_latency(args.data, args.out / "phase5_latency.png")
    plot_throughput(args.data, args.out / "phase5_throughput.png", capacity)
    print(f"wrote phase5_latency.png and phase5_throughput.png in {args.out}")


if __name__ == "__main__":
    main()

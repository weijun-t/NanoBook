"""plot_depth.py: draws one stock's order book from a snapshot CSV (cumulative shares by price).

What it does: Reads the CSV `nanobook_replay --symbol S --until T --snapshot-csv FILE` writes
  and draws one PNG: for each side, the shares available from the best price outwards, as a step
  line (bids to the left of the spread, asks to the right), with the best bid, best ask and spread
  marked. Usage:
    .venv/bin/python scripts/plot_depth.py SNAPSHOT.csv [--levels 50] [--out FILE.png]
  (default output: the CSV's name with .png).

Why it exists: plan §7.5: "that the book reconstructed from real ITCH data looks right". A
  real book is easy to recognise: a narrow spread, and depth that builds up away from it.

How it works: Only the `--levels` levels nearest the best price on each side are drawn. Real
  books also hold "stub" quotes far from the market (ITCH design §9.1), which would stretch the
  price axis from cents to thousands of dollars and squash the market into a dot; the subtitle
  says how many levels are left out and how far they reach. Colours: slots 1 and 2 of the
  dataviz reference palette (blue bids, orange asks), never red and green, which that palette
  reserves for status. The provenance lines of the CSV (input, time, commit) go in a footer.

Trade-offs: A static PNG for the report, light mode only. Needs matplotlib (requirements.txt).
"""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402  (must come after choosing the backend)
from matplotlib.ticker import FuncFormatter  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
SIDE_COLOURS = {"bid": "#2a78d6", "ask": "#eb6834"}  # reference palette slots 1 and 2

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
})


def read_snapshot(path: Path) -> tuple[dict, dict]:
    """The '# key: value' lines, and {'bid': [...], 'ask': [...]} rows, best first."""
    info, lines = {}, []
    with open(path, newline="", encoding="utf-8") as f:
        for line in f:
            if line.startswith("# "):
                key, _, value = line[2:].strip().partition(": ")
                info[key] = value
            else:
                lines.append(line)
    sides = {"bid": [], "ask": []}
    for r in csv.DictReader(lines):
        sides[r["side"]].append({"price": int(r["price_ticks"]) / 10_000, "qty": int(r["qty"])})
    return info, sides


def money(value: float, decimals: int) -> str:
    """$1,234.56, with the $ escaped: matplotlib reads text between two $ signs as maths."""
    return f"\\${value:,.{decimals}f}"


def steps(levels: list[dict]) -> tuple[list[float], list[int]]:
    """Prices and cumulative shares, best level first."""
    prices, totals, total = [], [], 0
    for level in levels:
        total += level["qty"]
        prices.append(level["price"])
        totals.append(total)
    return prices, totals


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("csv", type=Path, help="written by nanobook_replay --snapshot-csv")
    parser.add_argument("--levels", type=int, default=50, help="levels per side (default 50)")
    parser.add_argument("--out", type=Path, help="the PNG (default: the CSV's name with .png)")
    args = parser.parse_args()

    info, sides = read_snapshot(args.csv)
    if not sides["bid"] or not sides["ask"]:
        raise SystemExit(f"{args.csv}: one side of the book is empty, nothing to draw")
    shown = {s: levels[: args.levels] for s, levels in sides.items()}
    best_bid, best_ask = sides["bid"][0]["price"], sides["ask"][0]["price"]

    fig, ax = plt.subplots(figsize=(10, 4.6))
    for side, levels in shown.items():
        prices, totals = steps(levels)
        # From the spread outwards: the line starts at 0 shares at the best price.
        ax.step([prices[0]] + prices, [0] + totals, where="post" if side == "ask" else "pre",
                color=SIDE_COLOURS[side], linewidth=2)
        label = f"bids: {len(levels)} best levels" if side == "bid" else f"asks: {len(levels)} best levels"
        ax.annotate(label, (prices[-1], totals[-1]), xytext=(0, 6), textcoords="offset points",
                    ha="left" if side == "bid" else "right", color=INK_2, fontsize=8.5)

    spread_cents = round((best_ask - best_bid) * 100, 2)
    ax.axvspan(best_bid, best_ask, color=GRID, linewidth=0)
    ax.annotate(f"best bid {money(best_bid, 2)} · best ask {money(best_ask, 2)} · spread "
                f"{spread_cents:g}¢",
                ((best_bid + best_ask) / 2, 0), xytext=(0, -26), textcoords="offset points",
                ha="center", va="top", color=INK, fontsize=8.5, annotation_clip=False)

    ax.set_ylabel("shares available (cumulative from the best price)")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:,.0f}"))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: money(v, 2)))
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)

    symbol = info.get("symbol", "?")
    when = info.get("replayed until", "?")
    when = when[:8] if when[:2].isdigit() else when  # "16:00:00.000000000" -> "16:00:00"
    fig.suptitle(f"{symbol}'s order book at {when}, rebuilt from Nasdaq ITCH", x=0.01, y=0.98,
                 ha="left", fontsize=11, fontweight="bold", color=INK)
    rest = []
    for side in ("bid", "ask"):
        hidden = sides[side][args.levels:]
        if hidden:
            far = hidden[-1]["price"]
            rest.append(f"{len(hidden)} more {side} levels out to {money(far, 4)}")
    subtitle = "Not shown: " + "; ".join(rest) + " (stub quotes far from the market)" if rest else ""
    fig.text(0.01, 0.905, subtitle, ha="left", fontsize=8.5, color=INK_2)
    handles = [plt.Line2D([0], [0], color=SIDE_COLOURS[s], linewidth=2) for s in ("bid", "ask")]
    fig.legend(handles, ["bids (buyers)", "asks (sellers)"], loc="upper left", ncol=2,
               frameon=False, bbox_to_anchor=(0.003, 0.895), fontsize=8.5, labelcolor=INK_2)
    fig.text(0.01, 0.01, f"input {info.get('input', '?')} · commit {info.get('commit', '?')}",
             ha="left", va="bottom", fontsize=6.5, color=MUTED)
    fig.tight_layout(rect=(0, 0.06, 1, 0.85))
    out = args.out or args.csv.with_suffix(".png")
    fig.savefig(out, dpi=200)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# run_viewer_session.sh: what the viewer costs the pipeline, measured (viewer design §4, step 4).
#
# What it does: Builds the Release binaries with the viewer at the current commit, then runs the
#   paced latency run (nanobook_pipeline --latency, phase 5's method) at each rate, two rounds,
#   three ways each time:
#     off     no --view: the publisher's sink exactly as in phase 5 (the "before");
#     table   --view: the publisher also writes the L1 table, nobody reads it;
#     viewer  --view, with nanobook_view open on another core, reading the table ten times a
#             second (in a detached tmux session, so no terminal is needed).
#   Then writes summary.csv (one line per run: p50, p90, p99, p99.9, max, ring 2 full) and prints
#   it.
#     scripts/run_viewer_session.sh ITCH_FILE OUT_DIR [--cores A,B,C] [--view-core V]
#                                   [--rates "R1 R2 ..."] [--from HH:MM:SS] [--until HH:MM:SS]
#                                   [--allow-vm]
#   Defaults: cores 8,10,12 (the bare-metal session's isolated performance cores), the viewer on
#   core 2 (not isolated: it competes like a real user's terminal would), rates 1, 2 and 4 million
#   records/s (phase 5's published ones), the window 10:00:00-10:05:00. About 20 minutes (18
#   runs of about 40 s on WSL2; a WSL2 smoke run with a 10 s window and one rate took 4 minutes).
#
# Why it exists: Rule 22: the viewer adds work to the publisher thread, so it needs a before/after
#   table. The three ways separate the two costs: writing the table (off -> table) and a reader
#   pulling the table's cache lines away from the publisher's core (table -> viewer). Paced by a
#   fixed --rate, not --speed: at a sped-up ITCH clock the tail is queueing behind compressed
#   bursts (viewer report §7.2), which would hide the publisher's cost.
#
# How it works:
#   - Refuses to start with uncommitted tracked changes, in a virtual machine (rule 20) unless
#     --allow-vm, or without tmux.
#   - Order: for each round, for each rate: off, table, viewer. Interleaved, so a machine that
#     drifts during the session drifts under all three alike.
#   - Each run: `timeout 900`, `stdbuf -oL`, its own .txt (the tool's report) and .csv. For the
#     viewer runs, the screen is saved at the end (viewer_RATE_roundN.screen.txt): proof the
#     viewer was attached and reading.
#
# Trade-offs: Linux only; needs tmux. The laptop must be idle for the whole session. About 12 GB
#   of memory while a run holds the whole day.

set -u

usage() {
    echo "usage: scripts/run_viewer_session.sh ITCH_FILE OUT_DIR [--cores A,B,C] [--view-core V]" \
        "[--rates \"R1 R2 ...\"] [--from HH:MM:SS] [--until HH:MM:SS] [--allow-vm]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
FILE=$1
OUT=$2
shift 2
CORES=8,10,12
VIEW_CORE=2
RATES="1000000 2000000 4000000"
FROM=10:00:00
UNTIL=10:05:00
ALLOW_VM=0
while [ $# -gt 0 ]; do
    case $1 in
    --cores) CORES=${2:?}; shift 2 ;;
    --view-core) VIEW_CORE=${2:?}; shift 2 ;;
    --rates) RATES=${2:?}; shift 2 ;;
    --from) FROM=${2:?}; shift 2 ;;
    --until) UNTIL=${2:?}; shift 2 ;;
    --allow-vm) ALLOW_VM=1; shift ;;
    *) usage ;;
    esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
BIN=build/gcc-release
LOG=$OUT/progress.log
NAME=nanobook_session # the table: /dev/shm/nanobook_session
TMUX_SESSION=nanobook_viewer_session

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

# Runs one program, its output appended to file $1.
run() {
    local file=$1
    shift
    timeout 900 stdbuf -oL "$@" >> "$file" 2>&1
}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    log "refusing to start: tracked files have uncommitted changes (commit them first)"
    exit 1
fi
if grep -q hypervisor /proc/cpuinfo && [ "$ALLOW_VM" = 0 ]; then
    log "refusing to start: this is a virtual machine (rule 20); --allow-vm to try it anyway"
    exit 1
fi
if ! command -v tmux > /dev/null; then
    log "refusing to start: tmux is needed for the viewer runs (sudo apt-get install -y tmux)"
    exit 1
fi
log "building gcc-release with the viewer"
if ! { cmake --preset gcc-release -DNANOBOOK_BUILD_VIEWER=ON &&
    cmake --build build/gcc-release --target nanobook_pipeline nanobook_view; } \
    > "$OUT/build.txt" 2>&1; then
    log "build failed: see $OUT/build.txt"
    exit 1
fi
log "start: commit $(git rev-parse --short=12 HEAD), cores $CORES, viewer core $VIEW_CORE," \
    "rates $RATES, window $FROM-$UNTIL"

# One paced run. $1: off, table or viewer.
latency_run() {
    local how=$1 rate=$2 round=$3
    local base=$OUT/${how}_${rate}_round$round
    local view=()
    [ "$how" = off ] || view=(--view "$NAME")
    if [ "$how" = viewer ]; then
        tmux kill-session -t "$TMUX_SESSION" 2> /dev/null
        tmux new-session -d -s "$TMUX_SESSION" -x 132 -y 40 \
            "taskset -c $VIEW_CORE $BIN/nanobook_view $NAME"
    fi
    log "round $round: $how at $rate records/s"
    run "$base.txt" "$BIN/nanobook_pipeline" "$FILE" --cores "$CORES" --latency --rate "$rate" \
        --from "$FROM" --until "$UNTIL" --csv "$base.csv" "${view[@]}"
    log "  exit $?"
    if [ "$how" = viewer ]; then
        tmux capture-pane -t "$TMUX_SESSION" -p > "$base.screen.txt" 2>&1
        tmux kill-session -t "$TMUX_SESSION" 2> /dev/null
    fi
}

for round in 1 2; do
    for rate in $RATES; do
        for how in off table viewer; do
            latency_run "$how" "$rate" "$round"
        done
    done
done
rm -f "/dev/shm/$NAME"

# summary.csv: the latency line of each run's CSV, plus ring 2's full count from its report.
SUMMARY=$OUT/summary.csv
echo "how,rate,round,count,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,ring2_full" > "$SUMMARY"
for round in 1 2; do
    for rate in $RATES; do
        for how in off table viewer; do
            base=$OUT/${how}_${rate}_round$round
            # CSV columns: mode,scenario,book,tag,count,min,p50,p90,p99,p999,p9999,max,...
            line=$(grep '^latency,' "$base.csv" 2> /dev/null | tail -1)
            full=$(grep -o 'ring 2 full [0-9]*' "$base.txt" 2> /dev/null | tail -1 | awk '{print $4}')
            if [ -z "$line" ]; then
                echo "$how,$rate,$round,,,,,,,${full}" >> "$SUMMARY"
                continue
            fi
            echo "$line" | awk -F, -v how="$how" -v rate="$rate" -v round="$round" \
                -v full="$full" '{print how "," rate "," round "," $5 "," $7 "," $8 "," $9 "," $10 "," $12 "," full}' \
                >> "$SUMMARY"
        done
    done
done
column -s, -t < "$SUMMARY" | tee -a "$LOG"
log "done: $OUT"

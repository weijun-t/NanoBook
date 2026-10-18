#!/usr/bin/env bash
# run_latency_session.sh: one complete, recorded latency measurement session (phase 3).
#
# What it does: Rebuilds the Release binaries at the current commit, then runs, pinned to one
#   core and each as its own process: latency round 1 (every scenario on every book), throughput
#   mode, the matching Google Benchmark benchmarks (3 repetitions each), and latency round 2. Then
#   draws the charts from round 1. Everything lands in one output folder, with a timestamped
#   progress.log.
#     scripts/run_latency_session.sh OUT_DIR [--core C] [--ops N] [--preset P]
#   Defaults: core 2, 1,000,000 samples per run, preset gcc-release. Takes about 2 minutes on the
#   i7-14650HX.
#
# Why it exists: Every recorded latency number must come from a clean commit, in one session,
#   with its run-to-run noise known (docs/perf-log.md, "First full latency run"). This script is
#   that method written down, so the bare-metal run (latency-harness design §12, step 6) is the
#   same session as the WSL2 one.
#
# How it works:
#   - Refuses to start if tracked files have uncommitted changes: the binary's commit stamp
#     would say "-dirty", and the numbers couldn't be traced to a commit.
#   - Rebuilds first, so the binary can't be older than the commit (a step 5 run was thrown
#     away for exactly that).
#   - Each run: `timeout 600` (a stuck run can't hang the session), `stdbuf -oL` (output is
#     written line by line, so a killed run keeps what it printed), `taskset -c C` (pinned).
#   - Two latency rounds, so the round-to-round difference shows how much of each percentile
#     is noise. Compare them with the CSVs (round1.csv, round2.csv).
#
# Trade-offs: Linux only (taskset, stdbuf). The laptop must be idle for the whole session: other
#   work changes the numbers, and a busy session should be thrown away, not recorded. Before a
#   published run, quiet the machine first (design §7: a performance core, isolcpus, the
#   performance governor).

set -u

usage() {
    echo "usage: scripts/run_latency_session.sh OUT_DIR [--core C] [--ops N] [--preset P]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
OUT=$1
shift
CORE=2
OPS=1000000
PRESET=gcc-release
while [ $# -gt 0 ]; do
    case $1 in
    --core) CORE=${2:?}; shift 2 ;;
    --ops) OPS=${2:?}; shift 2 ;;
    --preset) PRESET=${2:?}; shift 2 ;;
    *) usage ;;
    esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
BIN=build/$PRESET
LOG=$OUT/progress.log

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

# Runs one program pinned to the core, its output appended to file $1.
pinned() {
    local file=$1
    shift
    timeout 600 stdbuf -oL taskset -c "$CORE" "$@" >> "$file" 2>&1
}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    log "refusing to start: tracked files have uncommitted changes (commit or stash them first)"
    exit 1
fi
log "building $PRESET"
if ! cmake --build --preset "$PRESET" > "$OUT/build.txt" 2>&1; then
    log "build failed: see $OUT/build.txt"
    exit 1
fi
log "start: commit $(git rev-parse --short=12 HEAD), core $CORE, $OPS samples per run"

log "latency round 1"
pinned "$OUT/round1.txt" "$BIN/nanobook_bench" --core "$CORE" --ops "$OPS" --csv "$OUT/round1.csv"
status=$?
log "  exit $status"
[ $status -eq 0 ] || exit 1

log "throughput mode"
pinned "$OUT/throughput.txt" "$BIN/nanobook_bench" --core "$CORE" --ops "$OPS" --throughput \
    --csv "$OUT/throughput.csv"
log "  exit $?"

# The Google Benchmark benchmarks that match the harness's scenarios, one process each.
for bench in 'BM_AddResting<%s>$' 'BM_Cancel<%s>$' 'BM_MatchOne<%s>$' 'BM_Sweep<%s>/10/' \
    'BM_LargeCancel<%s>/' 'BM_LargeAdd<%s>/' 'BM_Aged<%s>/'; do
    for book in ReferenceBook FastBook TickFastBook; do
        # shellcheck disable=SC2059  # the pattern is the format string on purpose
        filter="^$(printf "$bench" "$book")"
        log "google benchmark $filter"
        pinned "$OUT/gbench.txt" "$BIN/nanobook_microbench" --benchmark_filter="$filter" \
            --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
        log "  exit $?"
    done
done

log "latency round 2"
pinned "$OUT/round2.txt" "$BIN/nanobook_bench" --core "$CORE" --ops "$OPS" --csv "$OUT/round2.csv"
status=$?
log "  exit $status"
[ $status -eq 0 ] || exit 1

if [ -x .venv/bin/python ]; then
    .venv/bin/python -I scripts/plot_latency.py "$OUT/round1.csv" --prefix latency | tee -a "$LOG"
else
    log "no .venv: skipped the charts (python -m venv .venv && .venv/bin/pip install -r requirements.txt)"
fi
log "done: $OUT"

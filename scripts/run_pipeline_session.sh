#!/usr/bin/env bash
# run_pipeline_session.sh: one complete, recorded pipeline measurement session (phase 5 step 4).
#
# What it does: Rebuilds the Release binaries at the current commit, then runs, each as its own
#   process with the pipeline's threads pinned to three cores: the ring benchmark (one ring
#   between two cores) twice; the throughput run (the whole day on one thread, then on three)
#   twice; and the paced latency runs at each rate, twice. Everything lands in one output folder,
#   with a timestamped progress.log.
#     scripts/run_pipeline_session.sh ITCH_FILE OUT_DIR [--cores A,B,C] [--rates "R1 R2 ..."]
#                                     [--from HH:MM:SS] [--until HH:MM:SS] [--preset P]
#   Defaults: cores 2,4,6 (three different physical cores on this laptop: CPUs 2 and 3 are one
#   core's two hyperthreads), rates 500,000 to 8,000,000 records/s, the window 10:00:00-10:05:00
#   (5.6 million messages), preset gcc-release. About 20 minutes on the i7-14650HX.
#
# Why it exists: Every recorded number must come from a clean commit, in one session, with its
#   run-to-run noise known (as phase 3's run_latency_session.sh). This is phase 5's method
#   written down, so the bare-metal run is the same session as the WSL2 one.
#
# How it works:
#   - Refuses to start if tracked files have uncommitted changes, and rebuilds first, so the
#     binaries carry the commit they report.
#   - Each run: `timeout 900` (a stuck run can't hang the session), `stdbuf -oL` (output written
#     line by line, so a killed run keeps what it printed). The tools pin their own threads.
#   - Two rounds of everything, so the difference shows how much of each number is noise.
#
# Trade-offs: Linux only. The laptop must be idle for the whole session; a busy session should be
#   thrown away, not recorded. About 12 GB of memory while a run holds the whole day.

set -u

usage() {
    echo "usage: scripts/run_pipeline_session.sh ITCH_FILE OUT_DIR [--cores A,B,C]" \
        "[--rates \"R1 R2 ...\"] [--from HH:MM:SS] [--until HH:MM:SS] [--preset P]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
FILE=$1
OUT=$2
shift 2
CORES=2,4,6
RATES="500000 1000000 2000000 4000000 8000000"
FROM=10:00:00
UNTIL=10:05:00
PRESET=gcc-release
while [ $# -gt 0 ]; do
    case $1 in
    --cores) CORES=${2:?}; shift 2 ;;
    --rates) RATES=${2:?}; shift 2 ;;
    --from) FROM=${2:?}; shift 2 ;;
    --until) UNTIL=${2:?}; shift 2 ;;
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
FIRST_TWO=${CORES%,*} # "2,4,6" -> "2,4": the ring benchmark's two cores

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

# Runs one program, its output appended to file $1.
run() {
    local file=$1
    shift
    timeout 900 stdbuf -oL "$@" >> "$file" 2>&1
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
log "start: commit $(git rev-parse --short=12 HEAD), cores $CORES, rates $RATES, window $FROM-$UNTIL"

for round in 1 2; do
    log "round $round: ring benchmark (cores $FIRST_TWO)"
    run "$OUT/ringbench_round$round.txt" "$BIN/nanobook_ringbench" --cores "$FIRST_TWO"
    log "  exit $?"

    log "round $round: throughput, one thread then three"
    run "$OUT/throughput_round$round.txt" "$BIN/nanobook_pipeline" "$FILE" --cores "$CORES" \
        --throughput
    log "  exit $?"

    for rate in $RATES; do
        log "round $round: latency at $rate records/s"
        run "$OUT/latency_${rate}_round$round.txt" "$BIN/nanobook_pipeline" "$FILE" \
            --cores "$CORES" --latency --rate "$rate" --from "$FROM" --until "$UNTIL" \
            --csv "$OUT/latency_${rate}_round$round.csv"
        log "  exit $?"
    done
done
log "done: $OUT"

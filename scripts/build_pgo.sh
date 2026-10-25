#!/usr/bin/env bash
# build_pgo.sh: builds nanobook_replay with profile-guided optimisation, trained on the morning.
#
# What it does: Leaves build/gcc-release-pgo/nanobook_replay compiled with GCC's profile of a real
#   run (optimise design E4), ready for scripts/run_ab_session.sh --after-preset gcc-release-pgo.
#     scripts/build_pgo.sh ITCH_FILE [--until HH:MM:SS] [--core C] [--preset P]
#   Defaults: train on the day up to 12:00:00, pinned to core 2, preset gcc-release-pgo.
#
# Why it exists: PGO is two builds with a training run between them, done in a fixed order and
#   in one build folder. Written down once, so the training is the same every time it is redone,
#   on WSL2 now and on bare metal later.
#
# How it works:
#   1. scripts/itch_slice.py copies the day up to --until: the training data. It is the start of
#      the day, so the books build as on the real day, but it is not the data measured later
#      (the whole day), so PGO can't be flattered by training on its own test.
#   2. Configure with NANOBOOK_PGO=generate (counters compiled in) and build nanobook_replay.
#   3. Train: the same command the measurement runs (--throughput --latency), so the profile
#      covers the code that will be timed. GCC treats code the training never ran as cold and
#      optimises it for size, so training on other code paths would mislead it.
#   4. Reconfigure the SAME folder with NANOBOOK_PGO=use and rebuild: GCC finds each profile by its
#      object file's path, so a different folder would find none.
#   The training slice is deleted at the end; the profiles stay in build/P/pgo-data.
#
# Trade-offs: GCC only (CMakeLists.txt refuses PGO with Clang). The training run writes several
#   GB (the slice) and takes a few minutes; it needs no idle laptop, since only counts matter.

set -u

usage() {
    echo "usage: scripts/build_pgo.sh ITCH_FILE [--until HH:MM:SS] [--core C] [--preset P]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
FILE=$1
shift
UNTIL=12:00:00
CORE=2
PRESET=gcc-release-pgo
while [ $# -gt 0 ]; do
    case $1 in
    --until) UNTIL=${2:?}; shift 2 ;;
    --core) CORE=${2:?}; shift 2 ;;
    --preset) PRESET=${2:?}; shift 2 ;;
    *) usage ;;
    esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
BUILD=build/$PRESET
TRAIN=build/pgo-train.itch
LOG=build/pgo-build.log
: > "$LOG"
log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }
fail() {
    log "$1 (details in $LOG)"
    exit 1
}

# Reuse this tree's downloaded GoogleTest and Google Benchmark, if any (no new download).
DEP_ARGS=()
if [ -d build/gcc-release/_deps/googletest-src ]; then
    DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$ROOT/build/gcc-release/_deps/googletest-src")
fi
if [ -d build/gcc-release/_deps/benchmark-src ]; then
    DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_BENCHMARK=$ROOT/build/gcc-release/_deps/benchmark-src")
fi

log "1. training slice: $FILE up to $UNTIL"
python3 scripts/itch_slice.py "$FILE" "$TRAIN" --until "$UNTIL" | tee -a "$LOG" ||
    fail "slicing failed"

log "2. instrumented build (NANOBOOK_PGO=generate)"
rm -rf "$BUILD/pgo-data"
{ cmake --preset "$PRESET" -DNANOBOOK_PGO=generate "${DEP_ARGS[@]}" &&
    cmake --build --preset "$PRESET" --target nanobook_replay; } >> "$LOG" 2>&1 ||
    fail "instrumented build failed"

log "3. training run (the measurement's own command, on the slice)"
"$BUILD/nanobook_replay" "$TRAIN" --throughput --latency --core "$CORE" >> "$LOG" 2>&1 ||
    fail "training run failed"
PROFILES=$(find "$BUILD/pgo-data" -name '*.gcda' | wc -l)
[ "$PROFILES" -gt 0 ] || fail "the training wrote no profiles"
log "   $PROFILES profile files in $BUILD/pgo-data"

log "4. optimised build (NANOBOOK_PGO=use, same folder)"
{ cmake --preset "$PRESET" -DNANOBOOK_PGO=use "${DEP_ARGS[@]}" &&
    cmake --build --preset "$PRESET" --target nanobook_replay; } >> "$LOG" 2>&1 ||
    fail "optimised build failed"

rm -f "$TRAIN"
log "done: $BUILD/nanobook_replay (trained on the day up to $UNTIL)"

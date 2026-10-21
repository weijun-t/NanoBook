#!/usr/bin/env bash
# run_ab_session.sh: times one tool before and after a change, alternating, in one session.
#
# What it does: Builds TOOL twice, then runs it with each build in turn, so every phase 6
#   experiment gets its before/after numbers from the same session (optimise design §3.1):
#   - "before" is BEFORE_REF (a commit, branch or tag), built in a temporary git worktree;
#   - "after" is the current working tree, uncommitted changes included (an experiment is
#     measured before it is committed). Those changes are saved as OUT_DIR/after.diff (new
#     files included; nothing under docs/, which can't change a build and holds earlier
#     sessions' uncommitted output), so the numbers always name the exact code they came from.
#   Each run's output goes to OUT_DIR/before_roundN.txt or after_roundN.txt, with a timestamped
#   OUT_DIR/progress.log. "{out}" in the tool's arguments becomes that run's own path without
#   the extension (e.g. --csv {out}.csv), so runs never overwrite each other's files.
#     scripts/run_ab_session.sh BEFORE_REF OUT_DIR [--rounds N] [--preset P] [--timeout S]
#                               -- TOOL [ARGS...]
#     e.g. scripts/run_ab_session.sh HEAD docs/data/phase6_e1 -- nanobook_pipeline ITCH_FILE
#            --cores 2,4,6 --throughput
#   Defaults: 2 rounds, preset gcc-release, 1,800 s per run. With BEFORE_REF = HEAD and no
#   uncommitted changes, both sides are the same code: an A/A run, which measures the noise alone.
#
# Why it exists: README rules 10 and 22: no optimisation without a before/after table. Timing
#   "before" today and "after" tomorrow would blame the laptop's mood on the change.
#
# How it works:
#   - Builds both binaries first, then copies them to build/ab-bin/ (and removes the worktree),
#     so nothing is compiled while timing and both sides run exactly what was built.
#   - The worktree reuses this tree's downloaded GoogleTest and Google Benchmark sources
#     (FETCHCONTENT_SOURCE_DIR_*), so it doesn't download them again.
#   - Order A B, then B A, then A B... ("ABBA"): a machine that slowly warms up or cools down hits
#     both sides equally. Running all of A, then all of B, would blame that drift on the change.
#   - Each run is its own process under `timeout` and `stdbuf -oL` (a stuck run can't hang the
#     session; a killed run keeps what it printed). The tools pin their own threads (--core,
#     --cores), so the script doesn't.
#
# Trade-offs: Linux only. The laptop must be idle for the whole session; a busy session is thrown
#   away, not recorded. Refuses an OUT_DIR that already holds a session, so nothing is overwritten.

set -u

usage() {
    echo "usage: scripts/run_ab_session.sh BEFORE_REF OUT_DIR [--rounds N] [--preset P]" \
        "[--timeout S] -- TOOL [ARGS...]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
BEFORE=$1
OUT=$2
shift 2
ROUNDS=2
PRESET=gcc-release
TIMEOUT=1800
while [ $# -gt 0 ]; do
    case $1 in
    --rounds) ROUNDS=${2:?}; shift 2 ;;
    --preset) PRESET=${2:?}; shift 2 ;;
    --timeout) TIMEOUT=${2:?}; shift 2 ;;
    --) shift; break ;;
    *) usage ;;
    esac
done
[ $# -ge 1 ] || usage
TOOL=$1
shift
ARGS=("$@")

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
LOG=$OUT/progress.log
if [ -e "$LOG" ]; then
    echo "refusing: $OUT already holds a session (progress.log); choose a new folder" >&2
    exit 1
fi
WORKTREE=$ROOT/build/ab-before # our own scratch folder under build/ (ignored by git)
BIN=$ROOT/build/ab-bin

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

BEFORE_SHA=$(git rev-parse --verify --quiet "$BEFORE^{commit}") || {
    log "unknown commit: $BEFORE"
    exit 1
}
AFTER_SHA=$(git rev-parse HEAD)
git diff HEAD -- . ':!docs' > "$OUT/after.diff" # code only: docs/ can't change a build
# New files aren't in `git diff HEAD`: add each one as a diff against an empty file. Not docs/:
# it can't change a build, and it holds earlier sessions' uncommitted output.
git ls-files --others --exclude-standard -z -- . ':!docs' | while IFS= read -r -d '' f; do
    git diff --no-index -- /dev/null "$f" >> "$OUT/after.diff"
done
if [ -s "$OUT/after.diff" ]; then
    AFTER_TEXT="$(git rev-parse --short=12 HEAD) plus the uncommitted changes in after.diff"
else
    AFTER_TEXT="$(git rev-parse --short=12 HEAD), no uncommitted changes"
    rm "$OUT/after.diff"
fi
log "before: $(git rev-parse --short=12 "$BEFORE_SHA") ($BEFORE); after: $AFTER_TEXT"
if [ "$BEFORE_SHA" = "$AFTER_SHA" ] && [ ! -e "$OUT/after.diff" ]; then
    log "before and after are the same code: an A/A run, which measures the noise alone"
fi

# --- Build both sides ------------------------------------------------------------------------
log "building before (worktree at $WORKTREE)"
if git worktree list --porcelain | grep -qx "worktree $WORKTREE"; then
    git worktree remove --force "$WORKTREE"
fi
rm -rf "$WORKTREE"
if ! git worktree add --detach --quiet "$WORKTREE" "$BEFORE_SHA"; then
    log "could not create the worktree"
    exit 1
fi
DEPS=$ROOT/build/$PRESET/_deps
DEP_ARGS=()
if [ -d "$DEPS/googletest-src" ]; then
    DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$DEPS/googletest-src")
fi
if [ -d "$DEPS/benchmark-src" ]; then
    DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_BENCHMARK=$DEPS/benchmark-src")
fi
if ! (cd "$WORKTREE" && cmake --preset "$PRESET" "${DEP_ARGS[@]}" &&
    cmake --build --preset "$PRESET" --target "$TOOL") > "$OUT/build_before.txt" 2>&1; then
    log "before: build failed, see build_before.txt"
    exit 1
fi

log "building after (this working tree)"
if ! { { [ -d "build/$PRESET" ] || cmake --preset "$PRESET"; } &&
    cmake --build --preset "$PRESET" --target "$TOOL"; } > "$OUT/build_after.txt" 2>&1; then
    log "after: build failed, see build_after.txt"
    exit 1
fi

rm -rf "$BIN"
mkdir -p "$BIN/before" "$BIN/after"
cp "$WORKTREE/build/$PRESET/$TOOL" "$BIN/before/" && cp "build/$PRESET/$TOOL" "$BIN/after/" || {
    log "could not copy the binaries"
    exit 1
}
git worktree remove --force "$WORKTREE"

# --- Time them, alternating ------------------------------------------------------------------
# Runs one side once; "{out}" in the arguments becomes this run's own path prefix.
run() {
    local side=$1 round=$2
    local prefix=$OUT/${side}_round$round
    local args=("${ARGS[@]//\{out\}/$prefix}")
    log "round $round: $side"
    timeout "$TIMEOUT" stdbuf -oL "$BIN/$side/$TOOL" "${args[@]}" > "$prefix.txt" 2>&1
    log "  exit $?"
}

log "start: $TOOL ${ARGS[*]}; $ROUNDS rounds, order ABBA; the laptop must stay idle"
for round in $(seq 1 "$ROUNDS"); do
    if [ $((round % 2)) -eq 1 ]; then
        run before "$round"
        run after "$round"
    else
        run after "$round"
        run before "$round"
    fi
done
log "done: $OUT"

#!/usr/bin/env bash
# flamegraph.sh: profiles the real-day replay and draws where its time goes as a flame graph.
#
# What it does: Builds nanobook_replay with debug info, samples its call stacks with `perf record`
#   while it replays the day, folds the samples into stacks of inlined functions
#   (scripts/fold_stacks.py) and draws them with Brendan Gregg's flamegraph.pl.
#     scripts/flamegraph.sh ITCH_FILE OUT_DIR [--core C] [--stacks lbr|dwarf|fp] [--freq HZ]
#   Defaults: core 2, 1999 samples a second, and the first stack method this machine's perf can
#   do, in the order lbr, dwarf, fp (below). Writes OUT_DIR/flamegraph.svg, folded.txt,
#   replay.txt and progress.log. About 3 minutes, most of it the replay.
#
# Why it exists: plan §7.3 promises a flame graph, and phase 7 puts one in the README (polish
#   design §6.1): the first picture to show where a message's time goes.
#
# How it works:
#   - A flame graph needs each sample's call stack. There are three ways to get it:
#     lbr    The CPU records every call and return in its Last Branch Records (Intel, bare
#            metal only). Exact, leaf functions included, and nothing in the program changes.
#            Hardware cycles, user space only (cycles:u).
#     dwarf  perf copies the top of the stack at each sample and unwinds it later with the debug
#            info. Exact, and nothing changes, but it needs a perf built with libdw (Ubuntu's is;
#            WSL2's isn't) and writes about 8 KB per sample.
#     fp     The kernel follows the frame pointers (rbp, each frame's link to its caller). Works
#            everywhere, but only in a build that keeps them (-fno-omit-frame-pointer: one
#            register fewer, a push and pop per call), and GCC 14 still gives functions that
#            call nothing no frame, even with -mno-omit-leaf-frame-pointer (checked 2026-10-25):
#            a sample in IdTable::find, the hottest function, loses its caller. For trying the
#            script on WSL2 only.
#   - Build: lbr and dwarf profile gcc-release's program plus -g (build/gcc-release-g); fp a
#     frame-pointer build (build/gcc-release-fp). The script compares each one's instructions
#     with gcc-release's (disassemblies with addresses masked, since strings of different
#     lengths shift addresses) and reports how many lines differ: 0 for -g alone. It doesn't use
#     the gcc-profile preset (-O2): phase 6 measured that 2.4x slower, a different program.
#   - Record: `perf record -F HZ --call-graph METHOD`, the replay pinned with taskset. Its three
#     passes (count, parse only, timed replay) each show up as their own tower.
#   - Fold: `perf script -F ip,dso --show-mmap-events` -> fold_stacks.py (addr2line expands each
#     address into its inlined functions).
#   - Draw: flamegraph.pl from github.com/brendangregg/FlameGraph, cloned once into
#     build/FlameGraph at a pinned commit (not copied into this repo). The subtitle states the
#     machine, event, stack method and commit (rule 19).
#
# Trade-offs: Sampling shows shares of time, not latency: a box twice as wide had twice the
#   samples. Sampling slows the run (replay.txt's ns per message shows by how much), evenly enough
#   to keep the shares. Timer sampling (cpu-clock) in a VM interrupts the guest, so WSL2 graphs
#   are for trying the script; the published one comes from bare metal. The perf files stay in
#   build/flamegraph/ (tens to hundreds of MB, not for git).

set -u

usage() {
    echo "usage: scripts/flamegraph.sh ITCH_FILE OUT_DIR [--core C] [--stacks lbr|dwarf|fp]" \
        "[--freq HZ]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
FILE=$1
OUT=$2
shift 2
CORE=2
STACKS=
FREQ=1999
while [ $# -gt 0 ]; do
    case $1 in
    --core) CORE=${2:?}; shift 2 ;;
    --stacks) STACKS=${2:?}; shift 2 ;;
    --freq) FREQ=${2:?}; shift 2 ;;
    *) usage ;;
    esac
done
FLAMEGRAPH_COMMIT=41fee1f99f9276008b7cd112fca19dc3ea84ac32

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FILE=$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")
cd "$ROOT" || exit 1
mkdir -p "$OUT" build/flamegraph || exit 1
OUT=$(cd "$OUT" && pwd)
WORK=$ROOT/build/flamegraph
LOG=$OUT/progress.log
: > "$LOG"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }
fail() { log "FAILED: $*"; exit 1; }

# The event each method samples on: lbr needs hardware cycles in user space; the others use
# cycles where the CPU's counters can be read (bare metal), else the kernel's timer (WSL2).
if perf stat -e cycles -x, true 2>&1 | grep -q '^[0-9]'; then # a count: the counters work
    COUNTER=cycles
else
    COUNTER=cpu-clock
fi
event_for() { [ "$1" = lbr ] && echo cycles:u || echo "$COUNTER"; }
can_record() { # can this perf sample stacks with method $1 (and, for dwarf, unwind them)?
    if [ "$1" = dwarf ] && ! perf version --build-options | grep -q 'dwarf-unwind: *\[ on'; then
        return 1 # it would record the stack bytes but couldn't unwind them
    fi
    perf record -q -o "$WORK/probe.data" -e "$(event_for "$1")" --call-graph "$1" -- true \
        > /dev/null 2>&1
}
if [ -z "$STACKS" ]; then
    for method in lbr dwarf fp; do
        if can_record "$method"; then
            STACKS=$method
            break
        fi
    done
fi
case $STACKS in
lbr | dwarf) FLAGS="-g"; BUILD=build/gcc-release-g ;;
fp) FLAGS="-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"; BUILD=build/gcc-release-fp ;;
*) fail "this perf can't record call stacks (tried lbr, dwarf, fp)" ;;
esac
EVENT=$(event_for "$STACKS")
CALL_GRAPH=$STACKS
[ "$STACKS" = dwarf ] && CALL_GRAPH=dwarf,16384 # 16 KB of stack per sample: the replay is shallow

log "building gcc-release and $BUILD ($FLAGS)"
cmake --build --preset gcc-release --target nanobook_replay > "$WORK/build.txt" 2>&1 ||
    fail "gcc-release build (see $WORK/build.txt)"
cmake --preset gcc-release -B "$BUILD" -DCMAKE_CXX_FLAGS="$FLAGS" >> "$WORK/build.txt" 2>&1 &&
    cmake --build "$BUILD" --target nanobook_replay >> "$WORK/build.txt" 2>&1 ||
    fail "$BUILD build (see $WORK/build.txt)"
BIN=$BUILD/nanobook_replay
# The instructions, with every address and offset masked.
instructions() {
    objdump -d --no-show-raw-insn --no-addresses "$1" | tail -n +3 |
        sed -E 's/0x[0-9a-f]+|[0-9a-f]{4,}//g; s/<[^>]*>//g'
}
changed=$(diff <(instructions build/gcc-release/nanobook_replay) <(instructions "$BIN") |
    grep -c '^>')
log "instructions: $changed of $(instructions "$BIN" | wc -l) lines differ from gcc-release's"

if [ ! -d build/FlameGraph ]; then
    log "cloning FlameGraph at $FLAMEGRAPH_COMMIT"
    git clone -q https://github.com/brendangregg/FlameGraph build/FlameGraph || fail "git clone"
fi
git -C build/FlameGraph checkout -q "$FLAMEGRAPH_COMMIT" || fail "FlameGraph checkout"

log "recording: perf record -e $EVENT -F $FREQ --call-graph $CALL_GRAPH, core $CORE"
perf record -e "$EVENT" -F "$FREQ" --call-graph "$CALL_GRAPH" -o "$WORK/perf.data" -- \
    taskset -c "$CORE" "$BIN" "$FILE" --throughput --core "$CORE" \
    > "$OUT/replay.txt" 2> "$WORK/record.txt" || fail "perf record (see $WORK/record.txt)"
tail -1 "$WORK/record.txt" | tee -a "$LOG"
grep 'ns per message' "$OUT/replay.txt" | sed 's/^ */    /' | tee -a "$LOG"

log "folding"
perf script -i "$WORK/perf.data" -F ip,dso --show-mmap-events --no-inline \
    > "$WORK/perf_script.txt" 2> /dev/null || fail "perf script"
python3 -I scripts/fold_stacks.py "$WORK/perf_script.txt" "$BIN" --out "$OUT/folded.txt" |
    tee -a "$LOG"
[ -s "$OUT/folded.txt" ] || fail "fold_stacks.py wrote nothing"

COMMIT=$(git rev-parse --short=12 HEAD)
[ -n "$(git status --porcelain --untracked-files=no)" ] && COMMIT="$COMMIT-dirty"
SUBTITLE="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), $(uname -r),"
SUBTITLE="$SUBTITLE $EVENT at $FREQ Hz, stacks: $STACKS, core $CORE, commit $COMMIT" # ASCII only
build/FlameGraph/flamegraph.pl --title "nanobook_replay: a real Nasdaq day, where the time goes" \
    --subtitle "$SUBTITLE" --countname samples --width 1400 "$OUT/folded.txt" \
    > "$OUT/flamegraph.svg" || fail "flamegraph.pl"
log "done: $OUT/flamegraph.svg"

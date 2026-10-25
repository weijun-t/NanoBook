#!/usr/bin/env bash
# reproduce.sh: one command that takes a fresh clone to NanoBook's results (phase 7).
#
# What it does: Builds and tests the Release preset, runs the differential fuzzer, then measures
#   per-operation latency on one pinned core and prints it next to the published bare-metal
#   numbers. With --itch it also replays the real Nasdaq day (throughput and latency) and runs
#   the three-thread pipeline on it.
#     scripts/reproduce.sh [--itch FILE] [--core C] [--cores A,B,C] [--out DIR]
#   Defaults: core 2, pipeline cores 2,4,6, output in repro/. Without --itch it takes about a
#   minute on the i7-14650HX (the README promises under 10 minutes on a normal machine).
#
# Why it exists: The phase 7 exit criterion is "a stranger can build, test and reproduce the
#   numbers in under 10 minutes" (polish design §3). Before this script the steps were spread
#   over five reports.
#
# How it works:
#   - Checks the tools first (CMake >= 3.25, Ninja, gcc-14/g++-14, taskset, stdbuf) and says
#     what to install if one is missing. It never installs anything and never uses sudo.
#   - Every program's output goes to a file in the output folder, with `timeout` (a stuck run
#     can't hang the script) and `stdbuf -oL` (a killed run keeps what it printed).
#   - Quick level: `cmake --workflow --preset gcc-release` (configure, build, every test),
#     `nanobook_fuzz` (3 seeds x 10^7 random operations, fast book against reference book),
#     `nanobook_bench --book sorted` (the fast book, 1,000,000 timed samples per scenario).
#   - The comparison reads the published CSV (the release session's, docs/data/bare_metal/release/,
#     if present, else the first bare-metal session's phase 3)
#     and prints p50, p99 and p99.99 side by side for add, cancel, modify and match.
#   - Full level (--itch): `nanobook_replay --throughput --latency` once, then
#     `nanobook_pipeline --throughput` and `--latency --rate 1000000` on 10:00-10:05.
#
# Trade-offs: Linux only (taskset, stdbuf). Numbers depend on the machine: a laptop on battery,
#   a virtual machine (WSL2) or a busy machine will be slower, and the tools print a
#   "NOT FOR PUBLISHING" banner when they detect one. The script checks that the results come
#   out, not that they match; the comparison is for the reader to judge.

set -u

usage() {
    echo "usage: scripts/reproduce.sh [--itch FILE] [--core C] [--cores A,B,C] [--out DIR]" >&2
    exit 2
}

ITCH=
CORE=2
CORES=2,4,6
OUT=repro
while [ $# -gt 0 ]; do
    case $1 in
    --itch) ITCH=${2:?}; shift 2 ;;
    --core) CORE=${2:?}; shift 2 ;;
    --cores) CORES=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    *) usage ;;
    esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
if [ -n "$ITCH" ]; then
    [ -f "$ITCH" ] || { echo "no such file: $ITCH (scripts/fetch_itch.sh downloads it)" >&2; exit 1; }
    ITCH=$(cd "$(dirname "$ITCH")" && pwd)/$(basename "$ITCH")
fi
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
BIN=build/gcc-release
LOG=$OUT/progress.log
: > "$LOG"
START=$SECONDS

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

# Runs one program, its output to file $1; stops the script if it fails.
run() {
    local file=$1
    shift
    if ! timeout 900 stdbuf -oL "$@" > "$file" 2>&1; then
        log "FAILED: $* (see $file)"
        exit 1
    fi
}

# 1. Tools. `sort -V` compares version numbers ("3.28.3" against "3.25").
missing=
need() { command -v "$1" > /dev/null || missing="$missing $1"; }
need cmake
need ninja
need gcc-14
need g++-14
need taskset
need stdbuf
if [ -n "$missing" ]; then
    log "missing:$missing"
    log "on Ubuntu 24.04: sudo apt-get install -y cmake ninja-build g++-14 util-linux coreutils"
    exit 1
fi
cmake_version=$(cmake --version | head -1 | awk '{print $3}')
if [ "$(printf '3.25\n%s\n' "$cmake_version" | sort -V | head -1)" != 3.25 ]; then
    log "CMake $cmake_version is too old: 3.25 or newer is needed"
    exit 1
fi
log "tools: cmake $cmake_version, $(g++-14 --version | head -1)," \
    "commit $(git rev-parse --short=12 HEAD 2> /dev/null || echo unknown)"

# 2. Build and test.
log "1/3 build and test (cmake --workflow --preset gcc-release)"
run "$OUT/build_and_test.txt" cmake --workflow --preset gcc-release
log "    $(grep -E 'tests passed' "$OUT/build_and_test.txt")"

# 3. Differential fuzz: the fast book must match the reference book after every operation.
log "2/3 fuzz: 3 seeds x 10,000,000 operations, fast book against reference book"
run "$OUT/fuzz.txt" "$BIN/nanobook_fuzz" --ops 10000000 --seeds 3
grep -E 'PASS|FAIL' "$OUT/fuzz.txt" | sed 's/^/    /' | tee -a "$LOG"

# 4. Per-operation latency on one pinned core.
log "3/3 latency: fast book, 1,000,000 samples per scenario, core $CORE"
run "$OUT/latency.txt" taskset -c "$CORE" "$BIN/nanobook_bench" --book sorted --core "$CORE" \
    --csv "$OUT/latency.csv"
grep -E 'NOT FOR PUBLISHING' "$OUT/latency.txt" | head -1 | sed 's/^/    /' | tee -a "$LOG"

PUBLISHED=
for f in docs/data/bare_metal/release/phase3/round1.csv docs/data/bare_metal/phase3/round1.csv; do
    [ -f "$f" ] && { PUBLISHED=$f; break; }
done
{
    echo
    echo "Fast book, ns per operation. Yours: this machine. Published: $PUBLISHED"
    for key in cpu kernel 'isolated cores' commit; do
        grep -m1 "^# $key:" "$PUBLISHED" | sed 's/^#/   /'
    done
    # CSV columns: 1 mode, 2 scenario, 3 book, 4 tag, 7 p50, 9 p99, 11 p99.99 (all in ns).
    awk -F, '
        FNR == 1 { file++ }
        /^#/ || $1 != "latency" || $3 != "FastBook" || $2 != $4 { next }
        $2 !~ /^(add|cancel|modify|match)$/ { next }
        file == 1 { mine[$2] = sprintf("%8s %8s %8s", $7, $9, $11); order[++n] = $2 }
        file == 2 { pub[$2] = sprintf("%8s %8s %8s", $7, $9, $11) }
        END {
            printf "  %-8s | %8s %8s %8s | %8s %8s %8s\n", "", "yours", "", "", "published", "", ""
            printf "  %-8s | %8s %8s %8s | %8s %8s %8s\n", "op", "p50", "p99", "p99.99",
                "p50", "p99", "p99.99"
            for (i = 1; i <= n; i++)
                printf "  %-8s | %s | %s\n", order[i], mine[order[i]], pub[order[i]]
        }' "$OUT/latency.csv" "$PUBLISHED"
    echo
} | tee -a "$LOG"

# 5. Full level: the real day.
if [ -n "$ITCH" ]; then
    log "full 1/3: replay the day into the fast book, timed (core $CORE)"
    run "$OUT/day.txt" taskset -c "$CORE" "$BIN/nanobook_replay" "$ITCH" --throughput --latency \
        --core "$CORE" --csv "$OUT/day.csv"
    log "full 2/3: pipeline throughput, one thread then three (cores $CORES)"
    run "$OUT/pipeline_throughput.txt" "$BIN/nanobook_pipeline" "$ITCH" --cores "$CORES" \
        --throughput
    log "full 3/3: pipeline latency at 1,000,000 messages a second, 10:00-10:05"
    run "$OUT/pipeline_latency.txt" "$BIN/nanobook_pipeline" "$ITCH" --cores "$CORES" \
        --latency --rate 1000000 --from 10:00:00 --until 10:05:00 \
        --csv "$OUT/pipeline_latency.csv"
    log "    results: $OUT/day.txt, pipeline_throughput.txt, pipeline_latency.txt"
fi

log "done in $((SECONDS - START)) s; everything is in $OUT"

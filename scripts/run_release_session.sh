#!/usr/bin/env bash
# run_release_session.sh: the last bare-metal session, every published number at one commit.
#
# What it does: Runs, in order, at the current (clean) commit:
#   1. the stranger test: a fresh clone into an empty folder, then scripts/reproduce.sh, timed;
#   2. phase 3: scripts/run_latency_session.sh (per-operation latency, two rounds);
#   3. phase 4: the real day through the fast book, timed, twice;
#   4. phase 5: scripts/run_pipeline_session.sh (ring, throughput, paced latency, two rounds);
#   5. the flame graph: scripts/flamegraph.sh;
#   6. one A/B: phase 5's last commit (7d557fa) against this one on the real day, 2 rounds:
#      what phase 6 bought in total, both sides committed code;
#   then draws the A/B chart with scripts/plot_compare.py.
#     scripts/run_release_session.sh ITCH_FILE OUT_DIR [--core C] [--cores A,B,C] [--allow-vm]
#   Defaults: core 8, pipeline cores 8,10,12 (the isolated performance cores of the bare-metal
#   report §2). OUT_DIR for the published run: docs/data/bare_metal/release (scripts/reproduce.sh
#   compares a stranger's numbers with release/phase3/round1.csv). About 45 minutes. Every part
#   writes its own folder under OUT_DIR, with one OUT_DIR/progress.log for the whole session.
#
# Why it exists: Polish design §3.4. The phase 3-5 numbers were published at 943fe70, before E9b
#   was added back, so the README would mix commits; this re-runs them all at the release commit
#   in one boot, with the same commands as before, so nothing can be forgotten on the stick.
#
# How it works: Each part is the existing script or tool, unchanged, so the numbers are
#   comparable with the bare-metal report's. The script refuses to start on uncommitted changes
#   (every number must name its commit) or in a virtual machine (rule 20) unless --allow-vm is
#   given, for trying it on WSL2. The fresh clone is of this repository at this commit (not
#   GitHub), into an empty temporary folder, so it builds from nothing, downloads included.
#   A part that fails is logged and the session goes on to the next part.
#
# Trade-offs: Long, and the machine must stay idle throughout (the bare-metal report's
#   preparation: isolated cores, performance governor). Each part can also be run on its own.

set -u

usage() {
    echo "usage: scripts/run_release_session.sh ITCH_FILE OUT_DIR [--core C] [--cores A,B,C]" \
        "[--allow-vm]" >&2
    exit 2
}

[ $# -ge 2 ] || usage
FILE=$1
OUT=$2
shift 2
CORE=8
CORES=8,10,12
ALLOW_VM=0
PHASE5_END=7d557fa # the phase 5 report's commit: the code before any phase 6 change
while [ $# -gt 0 ]; do
    case $1 in
    --core) CORE=${2:?}; shift 2 ;;
    --cores) CORES=${2:?}; shift 2 ;;
    --allow-vm) ALLOW_VM=1; shift ;;
    *) usage ;;
    esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FILE=$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")
cd "$ROOT" || exit 1
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
LOG=$OUT/progress.log

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }
part() { # part NAME COMMAND...: runs one part, logs how it ended and how long it took
    local name=$1 start=$SECONDS
    shift
    log "== $name"
    if "$@" >> "$OUT/$name.log" 2>&1; then
        log "   done in $((SECONDS - start)) s"
    else
        log "   FAILED after $((SECONDS - start)) s: see $OUT/$name.log"
    fi
}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    log "refusing to start: tracked files have uncommitted changes (commit them first)"
    exit 1
fi
if grep -q hypervisor /proc/cpuinfo && [ "$ALLOW_VM" = 0 ]; then
    log "refusing to start: this is a virtual machine (rule 20); --allow-vm to try it anyway"
    exit 1
fi
log "release session: commit $(git rev-parse --short=12 HEAD), core $CORE, cores $CORES"

# 1. The stranger test: nothing reused from this tree but the commit.
stranger() {
    local dir start=$SECONDS status
    dir=$(mktemp -d) || return 1
    git clone -q "$ROOT" "$dir/NanoBook" && cd "$dir/NanoBook" &&
        scripts/reproduce.sh --core "$CORE" --out "$OUT/stranger"
    status=$?
    echo "fresh clone to results: $((SECONDS - start)) s"
    cd "$ROOT" && rm -rf "$dir"
    return $status
}
part stranger stranger
grep -h "fresh clone to results" "$OUT/stranger.log" | tee -a "$LOG"

part phase3 scripts/run_latency_session.sh "$OUT/phase3" --core "$CORE"

phase4() {
    mkdir -p "$OUT/phase4" || return 1
    for round in 1 2; do
        timeout 900 stdbuf -oL taskset -c "$CORE" build/gcc-release/nanobook_replay "$FILE" \
            --throughput --latency --core "$CORE" --csv "$OUT/phase4/round$round.csv" \
            > "$OUT/phase4/round$round.txt" 2>&1 || return 1
    done
}
part phase4 phase4

part phase5 scripts/run_pipeline_session.sh "$FILE" "$OUT/phase5" --cores "$CORES"

part flamegraph scripts/flamegraph.sh "$FILE" "$OUT/flamegraph" --core "$CORE"

part phase6_total scripts/run_ab_session.sh "$PHASE5_END" "$OUT/phase6_total" -- \
    nanobook_replay "$FILE" --throughput --latency --core "$CORE" --csv '{out}.csv'
if [ -x .venv/bin/python ]; then
    part phase6_chart .venv/bin/python -I scripts/plot_compare.py \
        --before "$OUT"/phase6_total/before_round[0-9].csv \
        --after "$OUT"/phase6_total/after_round[0-9].csv \
        --labels "end of phase 5" "release" --out "$OUT/phase6_total/compare.png" \
        --title "What phase 6 bought: FastBook per message, a real Nasdaq day (lower is better)"
fi
log "session done: $OUT"

# Handoff: FastBook `match` got slower (for Claude on WSL2)

> **Temporary file: delete it once the match regression is settled** (at the latest in the phase 7
> tidy-up). It is a handoff note, not documentation; the outcome belongs in the perf log and
> the final report.

Written 2026-10-25 by Claude on the USB stick, after the release session (commit `241ab76`,
pushed; data in `docs/data/bare_metal/release/`). Read this before phase 7 steps 7-9.

## What was seen
The phase 3 synthetic `match` scenario (one incoming order fully fills one resting order) is
slower at the release commit `f895be6` than at `943fe70`. Three separate measurements agree,
so it's real, not noise:

| measurement (bare metal, core 8)      | 943fe70  | f895be6 (release)    | change   |
|---------------------------------------|----------|----------------------|----------|
| latency harness p50, round 1 / 2      | 14.9 ns  | 16.9 / 17.4 ns       | +13-17%  |
| latency harness p99                   | 23.1 ns  | 19.4 / 19.8 ns       | better   |
| throughput mode (mean per op)         | 8.4 ns   | 9.4 ns               | +12%     |
| Google Benchmark `BM_MatchOne<FastBook>` | 8.86 ns (cv 0.2%) | 9.57 ns (cv 0.6%) | +8% |

Old: `docs/data/bare_metal/phase3/` (round1.csv, throughput.csv, gbench.txt).
New: `docs/data/bare_metal/release/phase3/` (same file names).

For contrast, at the same commits: `add` throughput 12.7 -> 12.0 ns (better; harness p50 19.4 ->
19.8 / 21.9 ns, round 2 only, maybe noise); cancel and modify unchanged. On the **real day**
FastBook is faster overall (75.4 -> 67.9 ns/msg) and real-day match p50 is 49 -> 43 ns. So the
regression shows up only in the synthetic scenario so far.

## What changed between the two commits
Only one hot-path change: **E9b** (8-byte tag entries in the ID table, re-added in `1754cdf`).
`git diff 943fe70 f895be6 -- include support` touches `id_table.hpp`, `fast_book.hpp` (every
`ids_.find`/`ids_.erase` now takes `id_of()`, a lambda that reads `pool_[slot].id` to confirm
a tag hit) and a comment in `huge_page_arena.hpp`. Nothing else in the core changed.

## Not yet known (don't explain it away)
Untested hypothesis: on a full fill, `remove_resting` calls `ids_.erase(order.id, id_of())`.
With E9b the erase reads a short tag and then confirms through `pool_[slot].id`. Before, the
entry held the full id. In a tiny synthetic book everything is in L1, so E9b's win (fewer cache
misses on a big table) can't show, and only its extra work (tag compare + confirm load, maybe a
longer dependency chain or a branch) remains. That would fit "slower in the micro, faster on the
real day". It is only a guess until it's measured.

## Suggested next step (ask the user before running; they say 'go' before each A/B)
1. Read `match`'s path: `handle(NewOrder)` -> `match` -> `remove_resting` -> `IdTable::erase`,
   and compare old/new `erase` in Compiler Explorer or `objdump -d` (count instructions/branches).
2. A/B on WSL2, only to see whether the gap is there at all (WSL2 numbers aren't publishable):
   `scripts/run_ab_session.sh 943fe70 OUT -- nanobook_bench ...` on the match scenario, or the
   Google Benchmark filter `BM_MatchOne`. `perf stat -e instructions,branches,branch-misses`
   on both binaries tells extra-work vs misprediction apart.
3. Decide with the user how the README reports it. Options: publish as measured with one line
   saying match is 8-17% slower in the micro-benchmark since E9b while the real day is 10% faster
   (honest, simplest), or find a fix (would need a new bare-metal run for phase 3 numbers).

## Status of everything else
Release session: all 7 parts done, flame graph by lbr, stranger test 55 s (fresh clone with
downloads, 790 tests), real day 68.0 / 67.7 ns/msg, pipeline p99 0.63-0.69 us at 1-4 M/s,
phase 6 total 86.4 -> 67.9 ns/msg (p50 90 -> 62 ns). Next: phase 7 steps 7-9 from this data.

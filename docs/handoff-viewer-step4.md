# Handoff: viewer step 4 measured on bare metal (for Claude on WSL2)

> **Temporary file: delete it once the step 4 numbers are in `docs/perf-log.md` and the viewer
> report.** It is a handoff note, not documentation.

Written 2026-10-26 by Claude on the USB stick, after `viewer-session.md`. Data: commit `5fd1c89`
on `ui` (pushed), folder `docs/data/bare_metal/viewer/` (`summary.csv`, `progress.log`, one
`.txt` + `.csv` per run, `viewer_*.screen.txt`). Every CSV names `v1.0.0-7-gf5aa78af79ba`.

## How it ran
Bare metal, `isolcpus=8-13 nohz_full=8-13 rcu_nocbs=8-13`, `performance` governor, turbo on,
laptop plugged in and idle (user confirmed). Defaults: pipeline cores 8,10,12, viewer core 2,
window 10:00:00-10:05:00 (5,641,949 records per run). 18 runs, all exit 0, 07:13-07:23. No
"NOT FOR PUBLISHING". Every viewer screen shows `/dev/shm/nanobook_session`, "done", 52,280,852
updates: the viewer was attached and reading.

## Fix needed: the script is not executable
`scripts/run_viewer_session.sh` is committed as mode `100644` (every other script is `100755`),
so `scripts/run_viewer_session.sh ...` fails with exit 126, "Permission denied". With the user's
OK it ran as `bash scripts/run_viewer_session.sh ...` (no tracked file touched, so the numbers
are still at `f5aa78a`). Fix: `git update-index --chmod=+x scripts/run_viewer_session.sh`.
Worth a CI check that every `scripts/*.sh` is executable.

## Results (ns, mean of rounds 1 and 2; in brackets: change from `off`)

| rate  | way    | p50       | p99       | p99.9     |
|-------|--------|-----------|-----------|-----------|
| 1 M/s | off    | 368       | 617       | 777       |
|       | table  | 375 (+8)  | 625 (+8)  | 780 (+3)  |
|       | viewer | 387 (+20) | 643 (+27) | 801 (+23) |
| 2 M/s | off    | 374       | 629       | 780       |
|       | table  | 373 (-0)  | 632 (+3)  | 782 (+2)  |
|       | viewer | 378 (+4)  | 639 (+10) | 792 (+12) |
| 4 M/s | off    | 378       | 684       | 884       |
|       | table  | 382 (+4)  | 686 (+2)  | 887 (+3)  |
|       | viewer | 386 (+8)  | 702 (+18) | 910 (+26) |

Ring 2 full: 0 in all 18 runs. Per-round numbers are in `summary.csv`.

- **Noise** (round 1 vs round 2, same way): 5-20 ns, up to 27 (4 M/s viewer p99.9). Largest at
  1 M/s: `off` itself moved 22 ns at p50, p99 and p99.9.
- **table vs off** (writing the table): +0 to +8 ns, inside the noise everywhere. No measurable
  cost.
- **viewer vs off** (a reader on core 2): each difference is near the noise, but **the viewer
  is slower than `off` in all 6 rate x round pairs at p50, p99 and p99.9** (p99: +6 to +45 ns,
  about 1-7%). Six of six in one direction: treat it as a small real cost, not noise. Untested
  hypothesis: the viewer's reads move the table's cache lines to core 2 (shared state), so the
  publisher's next write to a slot must take the line back (an RFO round trip). Not proven
  here; more rounds would size it. Don't explain it away.
- **Sanity vs phase 5** (`docs/data/bare_metal/release/phase5`, p99 round 1 / 2): 1 M/s
  638 / 631, 2 M/s 630 / 634, 4 M/s 680 / 686. Today's `off`: 605 / 628, 632 / 626, 687 / 681.
  Same machine behaviour.

## Not done here (yours)
- Before/after table in `docs/perf-log.md`; viewer report §1/§9; design-note status (step 4).
- Decide whether the viewer's cost needs a publisher-side fix (e.g. fewer writes per slot, or
  measure with `perf c2c` / more rounds first). The engine (`include/`, `src/`) stays unchanged.
- The executable-bit fix above.

## Memory files
Step 0's `cp` of `NanoBook-transfer/claude-memory/*.md` was **not** done as written: the transfer
copies of `phase3-bare-metal-pending.md` and `usb-stick-environment.md` were older than the
stick's (they lacked the release-session result, the isolcpus setup, the clang-rt fix). Only
`viewer-ui-branch.md` and its index line were added. Next time, copy only the new files or diff
first.

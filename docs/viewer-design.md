# Viewer design: a live L1 watchlist the engine never knows about

> **Status:** approved (2026-10-26): every decision in §9 as proposed. Work happens on the `ui`
> branch. Changes from here on are made by an edit plus a note in the commit message.
> Step 2: the table is in `support/` (`l1_table.hpp`, `l1_mapping.hpp`), next to the publisher's
> other sink (`binary_log.hpp`), so it reuses the recorder's buckets without `include/` depending
> on `support/`; the seqlock uses release stores / acquire loads instead of fences (§3.3).

The roadmap is done (`v1.0.0`, phases 0–7). Plan §1 allows one optional extra after phase 7: a
live view in a **separate process** that reads the publisher's output, as a TUI or a small web
page, with one hard condition: **the engine thread must never know it exists.** This note designs
the first version: an L1 watchlist (best bid and ask per stock) plus a pipeline-health panel.

---

## 0. Background in plain words

### 0.1 L1, L2, L3
- **L3** (order by order): every resting order. This is what NanoBook's books hold: ITCH is an
  order-by-order feed, and the book builder keeps every order in its ID table and level queue.
- **L2** (by price): total shares at each price. `depth()` computes it, but it returns a
  `std::vector` (allocates), so only tests and `plot_depth.py` use it.
- **L1** (top of book): best bid and best ask with their sizes. This is the one thing the engine
  publishes: one `TopOfBook` (64 bytes, `pipeline/records.hpp`) after every order message.

The viewer shows L1 because L1 is what already crosses ring 2. L2 would mean more work on the
engine thread per update (a measured experiment of its own, §10).

### 0.2 What reaches the publisher today
A `TopOfBook` holds `sequence`, `timestamp` (ITCH ns since midnight), `bid_price/qty`,
`ask_price/qty`, `sent` (time-stamp counter of the intended send time, 0 when not paced) and
`locate`. **No symbol:** names travel in Stock Directory (R) messages, which only the book builder
sees. But `nanobook_pipeline` already reads the whole directory before the run (the counting pass
that sizes the books, `itch/sizing.hpp`), so the tool knows every name before a record flows.

### 0.3 The problem a viewer has to solve
The pipeline publishes **millions of updates per second**; a terminal redraws maybe 10–30 times
per second. The viewer can't see every update and must not try: if the publisher waited for it,
a slow viewer would back up ring 2 and then stall the engine (breaking the point of plan §1).
So the viewer reads **the latest value per stock**, whenever it likes, and never slows the writer.

---

## 1. Goal and exit criteria

1. `nanobook_view` shows, live, one row per stock: symbol, bid size, bid, ask, ask size, spread,
   updates per second; sorted by activity, filterable by symbol.
2. A health panel: ITCH clock, replay speed, updates/s, end-to-end latency p50/p99/p99.9 over the
   last second (paced runs), and whether the publisher is alive.
3. **The engine is untouched:** no file under `include/nanobook/{core,fast,itch}` changes, and the
   pipeline's log checksum for the full day is identical with the viewer on and off (rule 11).
4. **The publisher's extra cost is measured:** the phase 5 latency run before and after, at the
   same rates, in `docs/perf-log.md` (rule 22). The viewer attached or not makes no difference
   to the pipeline (it only reads).
5. The viewer builds only with `-DNANOBOOK_BUILD_VIEWER=ON`; the default build and
   `reproduce.sh` are unchanged.

---

## 2. Shape

```
 feed thread ──ring 1──> engine thread ──ring 2──> publisher thread ──> binary log (as today)
                                                          │
                                                          └──> L1 table in shared memory  (new)
                                                                      │  (read-only mapping)
                                                                      v
                                                     nanobook_view (another process, FTXUI)
```

Three new pieces:
- `support/l1_table.hpp`: the table's layout and its seqlock (§3); `support/l1_mapping.hpp`
  puts it in named shared memory.
- A second sink in `nanobook_pipeline` (`--view NAME`): the publisher thread writes each update
  into the table after appending it to the log (§4). Feed and engine threads: no change.
- `tools/nanobook_view.cpp`: the terminal UI (§6).

The table, not the UI, is the interface. A web page later (§9 V1) would be one more reader of the
same table.

---

## 3. The L1 table in shared memory

### 3.1 Why not a ring
A ring (shared-memory SPSC, or UDP) delivers every update in order, so a slow reader either makes
the writer wait (forbidden) or overflows and must detect lost records. The viewer doesn't want
every update, only the latest per stock. **One slot per stock, overwritten in place**, gives
exactly that: the writer never waits, the reader samples when it wants, and there is nothing to
overflow. Updates between two redraws are skipped (conflation), which is what a screen needs.

### 3.2 Layout (POSIX `shm_open` + `mmap(MAP_SHARED)`, created by `nanobook_pipeline`)
- **Header**: magic, layout version, a run id (changes each run so the viewer notices a restart),
  state (starting / running / done), the counter's ticks per second (for converting latency),
  total updates, last ITCH time, and the latency histogram (§3.4).
- **Symbols**: 65,536 × 8 chars (512 KiB), one per locate, written before the state turns
  "running".
- **Slots**: 65,536 × 64 bytes (4 MiB, one cache line each, `alignas(64)`, `static_assert`ed),
  indexed by locate (a `uint16_t`, so every locate fits and no lookup is needed):
  `seq, sequence, timestamp, bid_price, bid_qty, ask_price, ask_qty, updates` (8 × 8 bytes).

About 4.5 MiB in all, mapped once at startup and pre-touched, so writing never page-faults.

### 3.3 The seqlock (one writer, any number of readers)
Each slot's `seq` is even when the slot is stable and odd while it is being written.
- **Write** (publisher): `seq = s + 1` (relaxed); store the fields (release, so the odd `seq` is
  visible before any of them); `seq = s + 2` (release). No read-modify-write, no lock: the
  publisher is the only writer.
- **Read** (viewer): `s1 = seq` (acquire); if odd, retry; load the fields (acquire, so the next
  load can't move before them); `s2 = seq` (relaxed); if `s1 != s2`, the writer was in the
  middle: retry. A reader that saw even one new field must then see `seq` changed.

Every field is a `std::atomic<std::uint64_t>`, so there is no data race in the C++ sense (a seqlock
over plain fields is undefined behaviour). No standalone fences: TSan can't follow them, and GCC
warns about them under `-fsanitize=thread` (which `-Werror` turns into a failed build). Atomics shared between processes must be
lock-free (address-free): `static_assert(std::atomic<std::uint64_t>::is_always_lock_free)`. On
x86-64 release stores and acquire loads compile to plain `mov`s: the cost is two extra stores
per update (the two `seq` stores).

### 3.4 Latency without coordinated omission
In paced runs the publisher already computes `now − sent` per update (the phase 5 latency). It
adds the value to a histogram in the header, in counter ticks, with the recorder's bucket scheme
(`latency_recorder.hpp`: 1 tick wide below 1,024, then 64 buckets per doubling; ~2,400 buckets of
`std::atomic<std::uint64_t>`, single writer, relaxed load + store). The viewer subtracts the last
frame's counts from this frame's and reads p50/p99/p99.9 from the difference, so it shows the last
second, not the whole run. `sent` is the *intended* send time (rule 18), so a stall still counts.

---

## 4. The publisher's new work, and its cost

Per update, after `log.append(t)`: one slot write (§3.3), two header stores (total, ITCH time),
and in paced runs one histogram bucket. Roughly 10–20 extra stores, all to cache lines the
publisher alone writes (the viewer only reads them; its reads can pull the lines away, which is
the cost to measure). Nothing changes on the feed or engine threads.

Measurement (rule 22): `nanobook_pipeline --latency` at the phase 5 rates (1, 2, 4 M/s), three
configurations: no `--view`; `--view` with no reader; `--view` with `nanobook_view` attached.
Report p50/p99/p99.9 and ring 2 full-counts. If p99 moves by more than the run-to-run noise, the
fix is on the publisher side (e.g. write the slot every N updates per stock), never on the engine.

---

## 5. Watching speed

A full day (368 million messages) goes through the three-thread pipeline in about 75 s unpaced
(~4.9 M/s): too fast to watch. The existing `Pacer` sends at a
fixed records-per-second rate, which flattens the day (the open's burst looks like lunch). The
viewer wants **ITCH-clock pacing**: record due at `start + (itch_time − first_itch_time) / speed`,
`--speed 1` real time, `--speed 60` an hour per minute. It stamps the intended time like `Pacer`
(so latency stays honest) and lives in `support/pacing.hpp` next to it: feed-thread tool code,
tested with the fake clock like `Pacer`. `--from HH:MM:SS` still replays the morning unpaced to
build the books first.

---

## 6. The viewer (`tools/nanobook_view.cpp`)

- **Opens** the table read-only (`shm_open(O_RDONLY)`, `PROT_READ`); waits if it doesn't exist
  yet; starts over when the run id changes; says "done" when the state does.
- **Every 100 ms** (10 Hz, §9 V4) it reads every slot whose `updates` changed, computes per-stock
  updates/s from the difference, and redraws.
- **Rows:** symbol, bid qty, bid, ask, ask qty, spread (ticks and bps), updates/s. Prices are
  integers in 1/10,000 of a dollar, printed with integer division (`165.0500`); only the bps
  column uses floating point (display, not matching, so rule 2 isn't touched).
  A side that's empty shows `—`; a crossed book (possible in auctions) is highlighted.
- **Keys:** `/` filter by symbol prefix, `s` cycle sort (activity / symbol / spread), `p` pin a
  symbol to the top, `q` quit.
- **Health panel:** ITCH clock (`09:31:04.123`), speed, updates/s, latency p50/p99/p99.9 (µs),
  publisher alive (total updates moved in the last second).
- It allocates and prints freely: it's another process, nowhere near the hot path.

**FTXUI** (C++, MIT, header-and-library, FetchContent like GoogleTest) at a pinned tag, only when
`NANOBOOK_BUILD_VIEWER=ON`.

---

## 7. Testing

- `l1_table` unit tests: layout `static_assert`s; write then read returns the same values; a
  slot with an odd `seq` (the writer in the middle, set by hand) is refused.
- Torn-read stress: one thread writes slots whose fields are all the same number, readers check
  every read they accept has all fields equal; under TSan in CI (rule 14).
- Two-process test: `fork()`, the child writes through `shm_open`, the parent reads.
- ITCH-clock pacer: fake clock, the same style as `Pacer`'s tests (due times, warm-up, stamps).
- Engine untouched: full-day log checksum with and without `--view` (§1.3); `git diff v1.0.0`
  shows no change under `include/nanobook/{core,fast,itch}`.
- The viewer itself: a smoke test that opens a table written by the test, renders one frame to a
  string (FTXUI can render to a `Screen` without a terminal) and checks the rows.

---

## 8. Plan, one commit per step

1. This note, approved.
2. `view/l1_table.hpp` + unit, torn-read and two-process tests.
3. `nanobook_pipeline --view NAME`: the second sink, symbols written at startup; checksum check.
4. Before/after latency table (§4) in `docs/perf-log.md`.
5. ITCH-clock pacing (`--speed`), with tests.
6. `nanobook_view` (FTXUI, CMake option, CI job that builds it), smoke test.
7. README: a "watch it run" section with one command and a screenshot; a viewer report.

---

## 9. Decisions (approved 2026-10-26)

| # | Question | Proposal |
|---|---|---|
| V1 | Terminal UI or web page? | **Terminal (FTXUI).** Same language, toolchain and CI; no server, no JavaScript, no second process to bridge shared memory to a socket. It runs over SSH on the bare-metal box. A web page can come later as one more reader of the same table. |
| V2 | Transport | **Latest-value table in shared memory with a seqlock** (§3), not a ring or UDP. |
| V3 | Where names come from | **The tool writes them at startup** from the directory it already reads to size the books; no new record type on ring 2. |
| V4 | Redraw rate | **10 Hz.** Fast enough to look live, cheap; 30 Hz is one constant if it feels slow. |
| V5 | Pacing | **ITCH-clock pacing with `--speed`** (§5), keeping `Pacer` as it is. |
| V6 | Repo | **This repo**, `NANOBOOK_BUILD_VIEWER=OFF` by default (the viewer depends on the table's layout; one commit changes both). |

---

## 10. Out of scope for this version

| Idea | Why not now |
|---|---|
| L2 depth ladder | Engine-side work per update; a phase-6-style experiment with before/after numbers, maybe for the watched stock only |
| Trade tape (executions) | Same: the engine sees executions but doesn't publish them |
| Charts of one stock over time | Needs history the table doesn't keep; the viewer could record its own samples later |
| Web page | V1: later, as another reader of the same table |
| "Phase 8" (strategies, signals) | Not part of an order book (fast-book design §12); a separate repo using NanoBook as a library |

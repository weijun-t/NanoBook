# Performance log

Every performance number in this project is recorded here with the environment it came from
(README rule 19). Each optimisation adds a before/after table (rule 22).

> **None of these numbers are for publishing.** They were measured on WSL2 (rule 20), and Google
> Benchmark reports averages, not tail latency (rule 17). They are only valid for comparing books
> **on the same machine**. Published p50/p99/p99.9 numbers come from the `rdtsc` harness (phase 3)
> on bare-metal Linux.

---

## 2026-10-16: Phase 1 baseline, reference book

The "before" column that the phase 2 fast book must beat (design §13).

### Environment

| | |
|---|---|
| CPU | Intel Core i7-14650HX, 12 cores / 24 threads (benchmark pinned to one core with `taskset -c 2`) |
| Caches | L1d 48 KiB, L2 2 MiB per core, L3 30 MiB shared |
| TSC | `constant_tsc`, `nonstop_tsc` present |
| OS / kernel | Ubuntu 24.04 on **WSL2**, kernel `6.18.54.1-microsoft-standard-WSL2` |
| Compiler | GCC 14.2.0 |
| Flags | `gcc-release` preset: `-O3 -DNDEBUG -std=c++23`, no `-march=native`, no LTO or PGO |
| Core isolation | None (`isolcpus` isn't available on WSL2) |
| Turbo / frequency scaling | Unknown: Windows controls it under WSL2 |
| Command | `taskset -c 2 ./build/gcc-release/nanobook_microbench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true` |
| Commit | Benchmarks added on top of `334b6fb` |

### Results (median of 5 repetitions; run-to-run variation below 1% for every row)

| Benchmark | What one operation is | Time per op | Ops per second | Allocations per op |
|---|---|---|---|---|
| `BM_AddResting` | Add a limit order that rests (100 price levels) | 58.9 ns | 17.0 M | **2.01** |
| `BM_Cancel` | Cancel a resting order (shuffled order) | 136.9 ns | 7.3 M | 0 |
| `BM_MatchOne` | An order that trades fully with one resting order | 31.5 ns | 31.7 M | 0 |
| `BM_Sweep/1` | One order sweeping 1 price level | 40.4 ns | 24.7 M | 0 |
| `BM_Sweep/10` | ... 10 levels | 385.6 ns | 2.6 M | 0 |
| `BM_Sweep/100` | ... 100 levels | 3.98 µs | 0.25 M | 0 |
| `BM_Mixed` | Seeded stream: 60% add, 30% cancel, 10% IOC | 83.6 ns | 12.0 M | **1.26** |

### What these numbers say

- **Adding allocates twice per order.** One `std::list` node for the order, one `std::map` node in
  the id lookup, plus a new `std::map` node whenever a price level is created (the extra 0.01).
  This is the README rule 1 violation that phase 2 must remove: its target is **exactly 0**.
- **Cancel is the slowest single operation**, about 2.3 times slower than add. Two `std::map`
  lookups (id → order, then price → level) jump between nodes all over the heap. Shuffled cancels
  touch memory in no particular order, so the CPU can't predict what to load next.
- **Sweeps grow linearly**: about 40 ns per level, whether 1, 10 or 100. Each level costs a
  `std::map` erase plus a `std::list` node free.
- **These numbers flatter the `std::map` book.** Every benchmark starts from a fresh book, so its
  nodes come straight from a fresh heap: close together in memory and still in cache. A book that
  has run for a whole trading day has its nodes scattered across memory, which means more cache
  misses. Tail latency in phase 3, and the full-day ITCH replay in phase 4, will show this better
  than any average.

---

## 2026-10-17: Fast book v1 vs. reference book

Phase 2 step 4 (`docs/fast-book-design.md` §11): order pool, intrusive queues and cached level
totals, with **temporary** parts still in place: price levels are a `std::map` per side
(`MapLevels`) and the id lookup is a `std::unordered_map` (`TemporaryIdIndex`). Both are replaced in
steps 5 and 6, so this is a midpoint, not the result.

### Environment
Same machine, OS, compiler, flags and command as the 2026-10-16 entry (pinned with `taskset -c 2`,
median of 5 repetitions). Commit: on top of `5320ddd`. Plugged in; load average about 1.

**Read this first:** today the *unchanged* reference book measured **1.6 to 1.9 times slower** than
on 2026-10-16 (for example, adding an order: 108.8 ns vs. 58.9 ns), so the machine itself was in a
different state (WSL2 gives no control over frequency or power). Numbers from different days can't
be compared. **Only compare the two columns below, which come from the same run.**

### Results

| Benchmark | Reference | Fast v1 | Fast / reference | Allocations per op (ref → fast) |
|---|---|---|---|---|
| Add a resting order | 108.8 ns | 36.6 ns | **3.0× faster** | 2.01 → **1.01** |
| Cancel | 239.2 ns | 99.8 ns | **2.4× faster** | 0 → 0 |
| Match one | 55.3 ns | 21.9 ns | **2.5× faster** | 0 → 0 |
| Sweep 1 level | 72.2 ns | 56.5 ns | 1.3× faster | 0 → 0 |
| Sweep 10 levels | 674.9 ns | 344.3 ns ⚠ | ~2.0× faster | 0 → 0 |
| Sweep 100 levels | 6.71 µs | 3.66 µs | 1.8× faster | 0 → 0 |
| Mixed stream | 97.8 ns | 48.5 ns ⚠ | ~2.0× faster | 1.26 → **0.66** |

Run-to-run variation was 1–4% for every row except the two marked ⚠ (19%), which are unreliable
today.

### What these numbers say
- **One of the two allocations per added order is gone.** The order now takes a pool slot instead
  of a `std::list` node. The one left is the `std::unordered_map` node in the temporary id index
  (plus the 0.01 for a new `std::map` level). Step 5 removes it, step 6 removes the level one.
- **Cancel got 2.4 times faster** even with both temporary parts still in. The id lookup is a hash
  table instead of a tree, and removing the order is an O(1) unlink instead of freeing a list
  node. It still does one tree lookup, to find the order's level in `MapLevels`; step 6 removes it.
- **Sweeps improved least.** Each emptied level is still a `std::map` erase, which step 6 removes.

---

## 2026-10-17: Fast book, temporary id index → flat IdTable (phase 2 step 5)

The only change: the fast book's id → slot lookup went from a `std::unordered_map`
(`TemporaryIdIndex`) to the flat open-addressing `IdTable` (`docs/fast-book-design.md` §5.1). The
price levels are still the temporary `MapLevels`.

### Method
To compare inside one session (see the warning in the previous entry), the previous commit
(`b9211ba`) was built from a separate git worktree, and the two `nanobook_microbench` binaries ran
**back to back, twice** (before, after, before, after), pinned with `taskset -c 2`, 5 repetitions
each, `--benchmark_filter=FastBook`. Same machine, compiler and flags as above. Run-to-run variation
was 0.3–4.7% for every row, and the two rounds agree to within 5%.

### Results (average of the two rounds' medians)

| Benchmark | Before (temporary index) | After (IdTable) | Change | Allocations per op |
|---|---|---|---|---|
| Add a resting order | 20.5 ns | 9.7 ns | **2.1× faster** | 1.01 → **0.01** |
| Cancel | 53.6 ns | 37.5 ns | **1.4× faster** | 0 → 0 |
| Match one | 12.6 ns | 7.8 ns | **1.6× faster** | 0 → 0 |
| Sweep 1 level | 30.8 ns | 25.0 ns | 1.2× faster | 0 → 0 |
| Sweep 10 levels | 280.5 ns | 226.2 ns | 1.2× faster | 0 → 0 |
| Sweep 100 levels | 2.99 µs | 2.46 µs | 1.2× faster | 0 → 0 |
| Mixed stream | 38.0 ns | 28.2 ns | **1.3× faster** | 0.66 → **0.05** |

(The machine was in a faster state than for the previous entry: the same v1 binary added an order
in 20.5 ns here vs. 36.6 ns there. Again: only same-session comparisons.)

### What these numbers say
- **The per-order allocation is gone.** Adding an order now allocates only when it creates a new
  price level (the 0.01: a `std::map` node in `MapLevels`). In the mixed stream, 0.05 allocations
  per operation are left, all of them new levels. Step 6 removes the last ones.
- **Add got twice as fast** because inserting into the id lookup no longer asks for memory: it
  writes one 16-byte entry into an array that was allocated at start-up.
- **Match one and sweeps got faster too**, though they never cancel: every fully filled resting
  order is removed from the id lookup, and removing from the flat table (a backward shift within
  one array) is cheaper than freeing a hash-table node.
- **Cancel improved least (1.4×):** the lookup got cheaper, but cancel still does a `std::map`
  lookup to find the order's level, which step 6 replaces.

---

## 2026-10-17: Price levels: sorted array and tick array (phase 2 step 6)

The temporary `std::map` levels (`MapLevels`) were replaced by `SortedLevels` (a sorted array per
side, binary search) in `FastBook`, and the `TickLevels` experiment was added as `TickFastBook`
(`docs/fast-book-design.md` §6). Two realistic benchmarks were added: an **aged** book (200,000
steered random inputs untimed, then 50 timed batches of 10,000 on the same book) and a **large**
book (100,000 resting orders on 10,000 levels; cancel or add 10,000).

### Method
The previous commit (`3628f9b`, `MapLevels`) was built from a separate git worktree. Both binaries
ran in **one session, twice**, pinned with `taskset -c 2`, **one process per benchmark** with output
written line by line, 3 repetitions each. Numbers are the average of the two rounds' medians.

Two problems with the first attempt, both fixed before these numbers:
- The sweep setup added 10,000 levels worst-first, so every insert shifted every level of the sorted
  array (about 50 million moves per book). The setup now adds levels best-first; the book built is
  the same.
- One whole run was lost: the machine slowed about 5× mid-run, a 30-minute timeout killed the run,
  and its piped output was still in a buffer. Hence one process per benchmark, line-buffered.

Benchmark band changed from [1, 1000000] to [1, 20000] (narrow enough for `TickLevels`; it doesn't
affect the other structures, which only store prices that have orders).

### Step 6 alone: `std::map` levels → sorted array (`FastBook`)

| Benchmark | Before (`MapLevels`) | After (`SortedLevels`) | |
|---|---|---|---|
| Add | 10.5 ns ⚠ | 11.7 ns | about the same |
| Cancel | 42.2 ns ⚠ | 38.5 ns | about the same |
| Match one | 10.0 ns ⚠ | 13.0 ns | inconclusive (the "before" rounds differed by 37%) |
| Mixed | 28.0 ns | 27.2 ns | about the same |
| Sweep 1 / 10 / 100 levels | 31.2 / 255.2 / 2525 ns | 18.3 / 137.3 / 1368 ns | **1.7× / 1.9× / 1.8× faster** |
| Allocations per mixed op | 0.053 | **0** | the last allocations are gone |

⚠ the "before" build's two rounds differed by 13–37% on these rows; the "after" build's by under
7% everywhere except the aged book (up to 15%). So only the sweeps are a clear speed change. With
100 levels a `std::map` stays in cache and is already fast; what step 6 buys is the last
allocations and the sweeps (no tree node freed per emptied level).

### All three books, same binary, same session

| Benchmark | Reference | FastBook | TickFastBook | Fast vs ref | Tick vs Fast |
|---|---|---|---|---|---|
| Add | 57.9 ns | 11.7 ns | 12.6 ns | 4.9× | 0.9× |
| Cancel | 136.0 ns | 38.5 ns | 11.8 ns | 3.5× | **3.3×** |
| Match one | 30.4 ns | 13.0 ns | 12.1 ns | 2.3× | 1.1× |
| Mixed | 81.5 ns | 27.2 ns | 18.4 ns | 3.0× | 1.5× |
| Sweep 1 level | 40.4 ns | 18.3 ns | 16.1 ns | 2.2× | 1.1× |
| Sweep 10 levels | 383.6 ns | 137.3 ns | 130.7 ns | 2.8× | 1.1× |
| Sweep 100 levels | 4.12 µs | 1.37 µs | 1.23 µs | 3.0× | 1.1× |
| Large book: cancel | 517.5 ns | 97.0 ns | 31.8 ns | 5.3× | **3.1×** |
| Large book: add | 258.9 ns | 71.2 ns | 18.1 ns | 3.6× | **3.9×** |
| Aged book | 87.3 ns | 35.9 ns | 28.8 ns | 2.4× | 1.2× |

Allocations per op: reference 2.01 (add), 1.26 (mixed), 2.0 (large add), 0.90 (aged); both fast
books **0** everywhere.

### What these numbers say
- **FastBook is 2.2–5.3× faster than the reference book on every benchmark, with zero
  allocations.** The biggest gains are on the large book, where the reference book's scattered tree
  nodes miss the CPU cache.
- **The tick array wins wherever a price has to be found**: cancel and the large book, 3–4× faster
  than the sorted array, because finding a level is one array index instead of a binary search.
  Where no lookup is needed (adding at a known level, matching, sweeping) the two are within about
  10%.
- **Design §6.3's condition is met**: the tick array is clearly faster on the realistic benchmarks
  (large book 3–4×, mixed 1.5×, aged 1.2×). So a moving-window tick array, which keeps that speed
  for any price band, is worth building in phase 6. It stays out of phase 2 as planned.

---

## 2026-10-18: Order slot 48 bytes vs 64 bytes (phase 2 step 7)

Design §4.1's [MEASURE] item. At 48 bytes (packed), 4 orders fill 3 cache lines, so half of them
straddle two lines. Padded to 64 bytes, every order is exactly one line, but the pool uses a third
more memory.

### Method
The 64-byte variant is this two-line patch to `include/nanobook/fast/order.hpp`, built in a
temporary git worktree on top of `8473976` (not committed):

```
-struct Order {
+struct alignas(64) Order {
-static_assert(sizeof(Order) == 48 && std::is_trivially_copyable_v<Order>);
+static_assert(sizeof(Order) == 64 && std::is_trivially_copyable_v<Order>);
```

Before timing, the patched build passed all 373 tests and 12 million fuzz operations against the
reference book (both fast-book variants). Then both builds ran in **one session, twice**, pinned
with `taskset -c 2`, one process per benchmark, 3 repetitions each, on an idle machine (an earlier
attempt was stopped and thrown away because the laptop was in use). Numbers are the average of
the two rounds' medians.

### Results
| Book | Benchmark | 48 bytes | 64 bytes | 64 vs 48 | Round-to-round noise |
|---|---|---|---|---|---|
| FastBook | Add | 11.9 ns | 12.2 ns | +2.0% | 2.2% |
| FastBook | Cancel | 38.8 ns | 39.6 ns | +2.1% | 2.5% |
| FastBook | Match one | 13.1 ns | 13.2 ns | +0.2% | 3.1% |
| FastBook | Mixed | 27.7 ns | 27.4 ns | -1.1% | 0.9% |
| FastBook | Sweep 1 level | 18.7 ns | 19.1 ns ¹ | +2.1% | 1.7% |
| FastBook | Sweep 10 levels | 139.7 ns | 146.1 ns | +4.6% | 2.9% |
| FastBook | Sweep 100 levels | 1.36 µs | 1.42 µs | +3.9% | 2.6% |
| FastBook | Large book: cancel | 95.0 ns | 94.4 ns | -0.6% | 3.5% |
| FastBook | Large book: add | 69.3 ns | 71.4 ns | +2.9% | 2.0% |
| FastBook | Aged book | 33.0 ns | 32.4 ns | -1.8% | 2.0% |
| TickFastBook | Add | 12.2 ns | 12.7 ns | +3.4% | 4.7% |
| TickFastBook | Cancel | 12.0 ns | 12.7 ns | +5.7% | 5.1% |
| TickFastBook | Match one | 12.5 ns | 12.2 ns | -2.0% | 3.7% |
| TickFastBook | Mixed | 18.0 ns | 17.9 ns | -0.7% | 7.2% |
| TickFastBook | Sweep 1 level | 15.9 ns | 15.9 ns | +0.3% | 0.8% |
| TickFastBook | Sweep 10 levels | 130.6 ns | 122.8 ns | -6.0% | 4.0% |
| TickFastBook | Sweep 100 levels | 1.20 µs | 1.17 µs | -2.3% | 1.4% |
| TickFastBook | Large book: cancel | 29.1 ns | 30.5 ns | +4.7% | 6.4% |
| TickFastBook | Large book: add | 17.1 ns | 17.9 ns | +4.5% | 8.0% |
| TickFastBook | Aged book | 26.8 ns | 25.8 ns | -3.6% | 4.3% |

¹ One round only: that run hit the 5-minute limit in round 2. Cause: a 100-sweep batch is only ~2 µs
of timed work, so Google Benchmark rebuilt the book hundreds of thousands of times (untimed but
slow), and the bigger 64-byte book pushed it over the limit. Fixed in this step: sweeps now run a
fixed 1,000 batches.

### What these numbers say
- **No consistent winner.** Every difference is within ±6%. 12 of the 20 rows are inside the
  round-to-round noise. The 8 beyond it are small and split 5 to 3: on `FastBook` 4 of 5 say
  64 bytes is slower (sweeps 1/10/100, large-book add; only mixed goes the other way), on
  `TickFastBook` they disagree (cancel slower, sweeps 10 and 100 faster). The 10-level sweep points
  opposite ways on the two books. (Corrected 2026-10-18: an earlier version said "three" rows
  were beyond the noise.)
- **So the order stays at 48 bytes**: the simpler choice, and a quarter less memory. Even the large
  book's orders (100,000 × 48 bytes, about 5 MB) fit in this CPU's 30 MB shared L3 cache, where a
  straddled line costs little; a book too big for L3, and phase 3's tail-latency numbers, are where
  a difference would show, if anywhere.

---

## 2026-10-18: The latency harness's timer (phase 3 step 1)

What the rdtsc timer itself measures on this machine (`docs/latency-harness-design.md` §3–§4),
before it times anything else. Same machine, OS and compiler as above; `gcc-release` and
`clang-release` builds; command `./build/gcc-release/nanobook_bench --core 2` (and unpinned).

| | Result |
|---|---|
| CPU flags | `constant_tsc`, `nonstop_tsc`, `rdtscp`, `tsc_known_freq` all present |
| Counter rate | **2.4192 ticks per ns** (a 2.419 GHz counter), every run |
| Spread between the 5 calibration rounds | 0.12–0.25 parts per million |
| Drift from start to end of a run | 0.035 parts per million |
| Timer overhead (empty start/stop pair), min | **26–28 ticks = 10.7–11.6 ns** |
| Timer overhead, median | **31 ticks = 12.8 ns** |
| Timer overhead, max | 12 µs to 3 ms, different every run |

**Assembly check** (GCC 14 `-O3`, a timed `FastBook::handle(Cancel)` call): `rdtscp`, `lfence`,
`call ...handle...`, `rdtscp`, `lfence`. The call stays between the two stamps and nothing from it
moved outside.

### What these numbers say
- **The counter is an excellent clock here.** Rounds agree to well under one part per million,
  and it doesn't drift during a run.
- **The timer costs about as much as the fast book's simplest operations** (adding an order
  averaged 11.7 ns in phase 2). So subtracting the overhead's minimum (design §13 Q2) is
  essential, and per-operation numbers near 10 ns are close to what this timer can resolve.
  Throughput mode (design §6.3) is the cross-check for those.
- **The max is the machine, not the timer:** an empty pair once took 3 ms, which can only be the
  thread being paused (WSL2's virtual machine, or Windows). That's the noise bare metal with an
  isolated core (design §7) should remove, and why outliers are reported, never hidden.

---

## 2026-10-18: First full latency run (phase 3 step 5)

Every scenario of the latency harness (`docs/latency-harness-design.md` §6) on all three books,
**1,000,000 samples each**, timed one operation at a time. WSL2, so **not for publishing**
(rule 20): every file and chart from this run carries the NOT FOR PUBLISHING banner.

### Method
- Same machine, OS and compiler as above. `gcc-release` build of commit `7dfab0466f00` (clean
  tree, rebuilt immediately before the run), pinned with `--core 2`, idle laptop.
- One session, each run its own process: latency round 1, throughput mode, the matching Google
  Benchmark benchmarks (3 repetitions, median), latency round 2. 16:29–16:31.
- Numbers below are the **average of the two rounds**. The timer overhead's minimum (28 ticks =
  11.6 ns) is subtracted from every sample. Counter 2.4192 ticks per ns, drift during each run
  under 0.03 parts per million.
- Data: `docs/data/phase3_wsl_round1.csv` (+ `_histogram.csv`), `phase3_wsl_round2.csv`,
  `phase3_wsl_throughput.csv`, `phase3_wsl_gbench.txt`. Charts: `docs/img/phase3_wsl_percentiles.png`,
  `docs/img/phase3_wsl_histograms.png` (from round 1).
- A first run 7 minutes earlier was thrown away: its binary had been built before step 3b was
  committed (its environment block said `51e4122aa9d0-dirty`). The C++ was the same, and its
  p50s agree with this run to a median 1.3% (at most 7.6%), but a recorded number must name a
  clean commit. The run script now refuses uncommitted changes and rebuilds first.

### Latency per operation (average of 2 rounds)

| Scenario | Book | p50 | p90 | p99 | p99.9 | p99.99 | mean |
|---|---|---|---|---|---|---|---|
| `add` | ReferenceBook | 65.3 ns | 94.8 ns | 131.7 ns | 223.6 ns | 4.70 µs | 72.3 ns |
| `add` | FastBook | 20.2 ns | 28.8 ns | 43.4 ns | 96.3 ns | 2.22 µs | 22.6 ns |
| `add` | TickFastBook | 19.1 ns | 27.7 ns | 38.4 ns | 73.2 ns | 2.28 µs | 21.6 ns |
| `cancel` | ReferenceBook | 137.2 ns | 165.8 ns | 207.1 ns | 615.5 ns | 6.65 µs | 142.0 ns |
| `cancel` | FastBook | 44.0 ns | 55.6 ns | 69.8 ns | 179.8 ns | 2.84 µs | 45.6 ns |
| `cancel` | TickFastBook | 23.5 ns | 31.2 ns | 40.5 ns | 98.8 ns | 2.36 µs | 26.2 ns |
| `modify` | ReferenceBook | 175.9 ns | 260.5 ns | 319.5 ns | 937.7 ns | 8.16 µs | 174.0 ns |
| `modify` | FastBook | 46.3 ns | 60.5 ns | 75.2 ns | 104.0 ns | 2.36 µs | 48.0 ns |
| `modify` | TickFastBook | 23.6 ns | 30.2 ns | 42.4 ns | 99.8 ns | 2.31 µs | 25.3 ns |
| `match` | ReferenceBook | 30.8 ns | 47.9 ns | 76.7 ns | 151.5 ns | 2.23 µs | 38.9 ns |
| `match` | FastBook | 15.7 ns | 21.9 ns | 29.4 ns | 71.3 ns | 2.23 µs | 18.1 ns |
| `match` | TickFastBook | 22.3 ns | 29.1 ns | 39.2 ns | 76.7 ns | 2.33 µs | 25.4 ns |
| `sweep10 (match)` | ReferenceBook | 407.8 ns | 464.8 ns | 638.2 ns | 3.41 µs | 36.85 µs | 433.6 ns |
| `sweep10 (match)` | FastBook | 146.8 ns | 164.8 ns | 229.4 ns | 551.2 ns | 8.93 µs | 153.8 ns |
| `sweep10 (match)` | TickFastBook | 132.7 ns | 169.9 ns | 224.8 ns | 455.5 ns | 9.43 µs | 145.3 ns |
| `large_cancel (cancel)` | ReferenceBook | 453.9 ns | 633.2 ns | 949.1 ns | 4.91 µs | 39.39 µs | 502.1 ns |
| `large_cancel (cancel)` | FastBook | 106.2 ns | 146.5 ns | 237.7 ns | 513.6 ns | 9.65 µs | 116.8 ns |
| `large_cancel (cancel)` | TickFastBook | 53.8 ns | 75.2 ns | 146.6 ns | 288.7 ns | 5.88 µs | 60.0 ns |
| `large_add (add)` | ReferenceBook | 206.1 ns | 608.0 ns | 922.4 ns | 3.69 µs | 22.75 µs | 307.4 ns |
| `large_add (add)` | FastBook | 83.0 ns | 112.0 ns | 190.8 ns | 378.6 ns | 8.82 µs | 92.8 ns |
| `large_add (add)` | TickFastBook | 27.7 ns | 32.0 ns | 96.1 ns | 234.8 ns | 3.41 µs | 32.5 ns |
| `deep_insert (add)` | ReferenceBook | 150.7 ns | 211.1 ns | 276.1 ns | 949.2 ns | 8.07 µs | 167.8 ns |
| `deep_insert (add)` | FastBook | **1.41 µs** | **2.79 µs** | **3.68 µs** | **10.49 µs** | **77.98 µs** | **1.54 µs** |
| `deep_insert (add)` | TickFastBook | 20.3 ns | 29.1 ns | 34.9 ns | 84.9 ns | 2.32 µs | 23.6 ns |
| `mixed` | ReferenceBook | 65.3 ns | 162.4 ns | 391.0 ns | 1.98 µs | 8.08 µs | 88.3 ns |
| `mixed` | FastBook | 32.2 ns | 66.1 ns | 143.8 ns | 398.5 ns | 5.11 µs | 40.6 ns |
| `mixed` | TickFastBook | 28.5 ns | 57.2 ns | 135.2 ns | 388.8 ns | 3.08 µs | 36.1 ns |

`mixed` is the whole stream ("all" tags); per-tag rows are in the CSV. No sample was dropped
(the thread never changed core), and **both fast books made 0 allocations while timed**, in
every scenario. The reference book allocates 2.0 per add, 3.0 per deep insert.

### How stable is each percentile on WSL2? (round 1 vs round 2)

| | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---|---|---|---|---|---|
| Median difference over the 27 runs | 1.7% | 2.7% | 4.8% | 14.5% | 15.7% | 88% | 4.3% |
| Largest difference | 13% | 30% | 32% | 72% | 114% | 189% | 15% |

### Throughput mode vs Google Benchmark (same session)

| Scenario (Google Benchmark name) | Book | Throughput mode | Google Benchmark | Difference |
|---|---|---|---|---|
| `add` (`BM_AddResting`) | ReferenceBook | 55.7 ns | 57.9 ns | -4% |
| `add` (`BM_AddResting`) | FastBook | 10.7 ns | 11.8 ns | -9% |
| `add` (`BM_AddResting`) | TickFastBook | 11.1 ns | 12.7 ns | -12% |
| `cancel` (`BM_Cancel`) | ReferenceBook | 142.2 ns | 136.8 ns | +4% |
| `cancel` (`BM_Cancel`) | FastBook | 37.8 ns | 39.2 ns | -4% |
| `cancel` (`BM_Cancel`) | TickFastBook | 10.4 ns | 11.8 ns | -12% |
| `match` (`BM_MatchOne`) | ReferenceBook | 30.3 ns | 30.0 ns | +1% |
| `match` (`BM_MatchOne`) | FastBook | 9.3 ns | 13.1 ns | -29% |
| `match` (`BM_MatchOne`) | TickFastBook | 12.5 ns | 12.1 ns | +3% |
| `sweep10` (`BM_Sweep`) | ReferenceBook | 444.6 ns | 378.9 ns | +17% |
| `sweep10` (`BM_Sweep`) | FastBook | 160.5 ns | 137.8 ns | +17% |
| `sweep10` (`BM_Sweep`) | TickFastBook | 141.5 ns | 141.4 ns | +0% |
| `large_cancel` (`BM_LargeCancel`) | ReferenceBook | 485.9 ns | 465.1 ns | +4% |
| `large_cancel` (`BM_LargeCancel`) | FastBook | 95.9 ns | 96.2 ns | -0% |
| `large_cancel` (`BM_LargeCancel`) | TickFastBook | 27.1 ns | 29.0 ns | -7% |
| `large_add` (`BM_LargeAdd`) | ReferenceBook | 266.1 ns | 235.4 ns | +13% |
| `large_add` (`BM_LargeAdd`) | FastBook | 68.2 ns | 68.6 ns | -1% |
| `large_add` (`BM_LargeAdd`) | TickFastBook | 16.2 ns | 16.3 ns | -1% |
| `mixed` (`BM_Aged`) | ReferenceBook | 82.4 ns | 82.6 ns | -0% |
| `mixed` (`BM_Aged`) | FastBook | 29.0 ns | 34.2 ns | -15% |
| `mixed` (`BM_Aged`) | TickFastBook | 26.2 ns | 28.0 ns | -6% |

### What these numbers say
- **p50 to p99 are trustworthy even on WSL2** (rounds agree to a few %); **p99.9 and beyond
  are mostly the machine.** Almost every simple operation, on every book, has its p99.99 at
  2.2–2.4 µs, including the 19 ns tick-array add: about 1 operation in 10,000 is interrupted
  for ~2 µs whatever it is doing. That floor, and the max (tens of µs to ms, different every
  round), is WSL2/Windows, and is what bare metal with an isolated core (design §7) should
  remove. Only bare-metal tails can be published or compared.
- **The deep-insert weakness, measured** (phase 2 report §5.5): adding a new price at a random
  depth of a 5,000–10,000-level book costs the sorted array **1.41 µs at p50 and 3.68 µs at
  p99**, against 151 ns / 276 ns for the `std::map` reference book and 20 ns / 35 ns for the tick
  array. Every such insert copies, on average, about 120 KB of levels. In every other scenario
  FastBook beats the reference book at every percentile up to p99.9 (2–5× at p50). Whether
  deep inserts are common in real order flow is phase 4's question; the fixes are phase 6's.
- **The fast books' tails are tighter, not only their medians:** at p99, FastBook is 2.7–4.8×
  below the reference book on cancel, modify, the large book and the mixed stream.
- **The tick array's advantage is lookups** (cancel 23.5 vs 44.0 ns, large-book add 27.7 vs
  83.0 ns at p50), as phase 2's averages showed; on `match` it is slower than FastBook
  (22.3 vs 15.7 ns), also seen in phase 2 (12.1 vs 13.0 ns there, within noise).
- **Per-operation timing costs about 6–10 ns on top of the work**: FastBook add is 10.7 ns per
  operation in throughput mode but 20.2 ns at p50 one by one (after subtracting the timer's own
  11.6 ns). That's the fences stopping the CPU from overlapping one operation with the next
  (design §3.4). It applies equally to every book, so comparisons stand, but single-operation
  numbers under ~30 ns overstate the cost of an operation in a stream.
- **Throughput mode agrees with Google Benchmark** within ±15% on 17 of 21 rows, 11 of them
  within ±5%. Partly explained differences: `sweep10` (+17% for the reference book and
  FastBook) uses a 10,000-level book in the harness against 1,000 levels in `BM_Sweep`; `mixed`
  vs `BM_Aged` (FastBook −15%) replay the same seeded stream, but Google Benchmark times its
  first 500,000 inputs and the harness 1,000,000, so the books differ over the second half.
  **Not explained: FastBook `match`,
  9.3 ns here vs 13.1 ns in Google Benchmark (−29%), in both runs today**, while the reference
  and tick books agree to 1–3%. Same inputs and book; a code-layout or inlining difference
  between the two binaries is the likely cause. To check with `perf` on bare metal (phase 6
  tooling), not by guessing.

---

## 2026-10-20: A real Nasdaq day through FastBook (phase 4 step 6c)

The first timing on real order flow (`docs/itch-design.md` §9): the whole of 30 Jan 2019
(368,366,634 ITCH messages, 8,713 stocks) replayed into one FastBook per stock. WSL2, so **not
for publishing** (rule 20): every file from this run carries the NOT FOR PUBLISHING banner.

### Method
- Same machine, OS and compiler as above: i7-14650HX, Ubuntu 24.04 on WSL2 (kernel
  `6.18.54.1-microsoft-standard-WSL2`), GCC 14.2.0, `gcc-release` (`-O3 -DNDEBUG`), commit
  `c13e5bb7252e` (clean tree, rebuilt immediately before), pinned to core 2 (not isolated),
  plugged in, idle laptop. Counter 2.4192 ticks per ns; timer overhead minimum 28 ticks (11.6 ns),
  subtracted from every latency sample; drift under 0.2 parts per million.
- Command, twice, 19:22–19:26: `nanobook_replay data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50
  --throughput --latency --core 2 --csv docs/data/phase4_wsl_roundN.csv`. Each run: the counting
  pass and sizing (untimed), a parse-only pass, a parse + FastBook pass (throughput), and a
  third pass timing **every 8th order message** (45,389,776 samples). Checks off. The file is
  read in 1 GiB windows whose pages are loaded before the clock starts (design §4), so the disk
  is not timed.
- Numbers below are the **average of the two rounds**; the round-to-round difference is in
  brackets. Data: `docs/data/phase4_wsl_round1.txt`, `.csv`, `_histogram.csv` (and round 2).

### Throughput (every message of the day, parsing included)

| | ns per message | Messages per second |
|---|---|---|
| Parse only | 3.1 (7%) | about 320 M |
| Parse + FastBook | **104.1** (3.4%) | **9.6 M** |
| The books' share | 101.0 | 97% of the time |

### Latency per order message, FastBook (every 8th; ns)

| Kind (ITCH) | Samples | p50 | p90 | p99 | p99.9 | p99.99 | mean |
|---|---|---|---|---|---|---|---|
| add (A, F) | 20,584,856 | 98.3 (3%) | 177.4 | 299.7 (7%) | 482.1 | 10,389 (15%) | 111.3 |
| execute (E, C) | 1,030,612 | 105.6 (5%) | 261.9 | 461.7 (10%) | 727.3 | 10,197 | 135.3 |
| delete (D) | 19,786,668 | 111.0 (5%) | 269.3 | 461.1 (9%) | 722.5 | 11,637 (15%) | 140.9 |
| partial cancel (X) | 584,111 | 54.5 (3%) | 193.2 | 371.8 (9%) | 615.5 | – | 88.2 |
| replace (U) | 3,403,529 | 168.8 (3%) | 333.4 | 539.9 (7%) | 915.0 | 12,994 | 196.9 |
| **all** | 45,389,776 | **104.8** (5%) | 240.3 | **428.6** (8%) | 690.3 (10%) | 11,241 (18%) | 130.9 |

p99.99 is left out where fewer than 1,000,000 samples back it (as in phase 3). No sample was
dropped or clamped.

### Against phase 3 (one book, made-up scenarios, same machine, 2026-10-18)

| | Phase 3 FastBook p50 | Real day p50 | Ratio |
|---|---|---|---|
| Add a resting order | 20.2 ns (`add`) | 98.3 ns | 4.9x |
| Remove a resting order | 44.0 ns (`cancel`) | 111.0 ns (delete) | 2.5x |
| Shrink an order, keep its place | 46.3 ns (`modify`, half of them) | 54.5 ns (partial cancel) | 1.2x |
| Whole stream, mean | 29.0 ns (`mixed`, throughput mode) | 104.1 ns (throughput) | 3.6x |

### What these numbers say
- **The parser is 3% of the time; the books are 97%.** Decoding a message costs about 3 ns
  (one load and a byte swap per field, design §5), applying it to the book about 100 ns. Speed
  work belongs in the book.
- **The same book is 2.5–5x slower per operation on the real day than in phase 3.** Phase 3 timed
  one book that stayed in the CPU's caches. The real day jumps between 8,713 books whose memory
  (about 2.5 million order slots, plus id tables and levels) is far bigger than the 30 MB L3
  cache, so most messages likely start with cache misses. The step 6 flow analysis also counts
  about 40 GB of level shifting over the day (design §9.1), roughly 1 s of copying at 40 GB/s. These
  are the likely causes, not yet separated: that needs hardware counters (`perf stat`
  cache-misses), so bare metal (phase 6 tooling).
- **Partial cancels are the cheapest (54.5 ns)**: find the order, shrink it in place, no level
  change. **Replaces are the dearest (168.8 ns)**: a removal and an insert, often at a new price.
- **Timing a message costs more here than in phase 3.** The latency mean (130.9 ns, order
  messages only) is about 27 ns above the throughput mean (104.1 ns, all messages, parsing
  included); phase 3 saw 6–10 ns. The timer's fences stop the CPU starting the next message's
  memory reads early, and with cache misses that overlap is worth more.
- **p99.99 is about 11 µs, against phase 3's ~2.3 µs floor.** Leading hypothesis, not yet tested:
  **first-touch page faults in the sorted-level arrays.** `SortedLevels` only `reserve()`s its two
  arrays (161 MB for the day's 2.5 million order slots), so a page is first written mid-replay;
  the order pool and the id table are zero-filled at construction, so theirs aren't. At one
  fault per newly used 4 KiB page, that's tens of thousands of faults over 363 million order
  messages, about 1 in 10,000: the p99.99 level. Test: touch the level arrays at construction
  (as the latency recorder does) and compare, before/after (rule 10), in phase 6.
- **Round-to-round agreement is like phase 3's:** throughput 3.4%, p50 3–5%, p99 7–10%, p99.99
  up to 18%. p50 and p99 are trustworthy on WSL2; the far tail is not, and only bare metal can
  settle it.

---

## 2026-10-21: Three threads, one ring and the paced pipeline (phase 5 step 4b)

The first numbers for the three-thread pipeline (`docs/pipeline-design.md` §9): one ring on its
own, the whole day unpaced (one thread against three), and end-to-end latency at five feed rates.
WSL2, so **not for publishing** (rule 20): every file from this run carries the banner.

### Method
- Same machine, OS and compiler as above: i7-14650HX, Ubuntu 24.04 on WSL2, GCC 14.2.0,
  `gcc-release` (`-O3 -DNDEBUG`), commit `9c2b233b68f2` (clean tree, rebuilt by the script),
  idle laptop, plugged in. Threads pinned to CPUs 2 (feed), 4 (engine), 6 (publisher): three
  different physical cores, not isolated, spinning only. Counter 2.4192 ticks per ns.
- One command, 13:01–13:15: `scripts/run_pipeline_session.sh
  data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50 docs/data/phase5_wsl`. Two rounds of:
  `nanobook_ringbench --cores 2,4`; `nanobook_pipeline --throughput` (the whole day, logs to
  `/dev/null`); `nanobook_pipeline --latency` at 0.5, 1, 2, 4 and 8 M records/s, paced from
  10:00:00 to 10:05:00 (5,641,949 records, every update timed; the day before 10:00 is the
  unpaced warm-up, then a 100 ms settle). The whole file is in memory before any clock starts.
- Latency is from each record's **intended** send time on the feed core to its update reaching
  the publisher core (coordinated omission counted, rule 18). Data: `docs/data/phase5_wsl/`.
  Numbers below are the average of the two rounds, with the round-to-round difference in
  brackets, or both rounds as "round 1 / round 2" where they disagree too much to average.

### One ring between two cores (`nanobook_ringbench`, 8-byte records, 65,536 slots)

| | Result |
|---|---|
| Throughput, 100 M records | **5.9 ns per record, 170 M records/s** (1.2%) |
| Round trip A → B → A, min | 129 ns |
| Round trip p50 | **185 ns** (6%): one way about 90 ns |
| Round trip p90 / p99 | 229 ns / 283 ns (17%) |
| Round trip p99.9 / p99.99 | 2.6 / 1.0 µs, then 12.2 / 11.5 µs |
| Round trip max | 1.1 ms / 17.7 ms |

### The whole day, unpaced (368,366,634 messages, logs to `/dev/null`)

| | Time | ns per message | Messages per second |
|---|---|---|---|
| One thread (`run_sequential`) | 63.9 s (1.6%) | **173.5** | **5.8 M** |
| Three threads | 93.7 s / 81.3 s (15%) | 237.4 | 4.2 M (0.69x / 0.78x) |

Ring 1 (feed → engine) was found full about 1.66 billion times, **4.5 times per message**;
ring 2 (engine → publisher) never.

### End-to-end latency against feed rate (ns unless marked)

| Rate (records/s) | min | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| 0.5 M | 183 | **515** (1%) | 833 | **65 µs** (27%) | 1.6 ms / 5.0 ms | 8.9 ms / 16.1 ms |
| 1 M | 180 | **507** (3%) | 809 | **68 µs** (15%) | 11.4 ms / 0.65 ms | 15.6 ms / 2.8 ms |
| 2 M | 178 | **502** (0.3%) | 799 | **72 µs** (1%) | 446 µs / 369 µs | 0.82 ms / 1.08 ms |
| 4 M | 170 | **657** (3%) | 11.7 µs | 270 µs / 194 µs | 707 µs / 828 µs | 0.82 ms / 1.01 ms |
| 8 M | 4–7 µs | 285 ms | 529 ms | 584 ms | 587 ms | 583 ms / 593 ms |

No sample was clamped or refused. For scale: the real market sent this window at an average of
18,800 messages per second, so 0.5 M/s is 27 times the real average (bursts are far higher).

### What these numbers say
- **The fastest a record can cross the pipeline is about two core-to-core hops.** The latency
  minimum (170–183 ns) is close to the ring's round trip (129–185 ns), and a record makes two
  one-way hops (feed → engine → publisher). p50 at the lower rates is about 500 ns: the two hops
  plus the engine's book work (about 100 ns per order message on the real day, phase 4) plus the
  encoding of the records on each side.
- **Up to 2 M records/s, p50 and p90 don't move; at 4 M they start to.** p50 rises 30% and p90
  jumps from 0.8 to 11.7 µs: the engine is busy about 90% of the time, so records start to queue.
- **Above the pipeline's capacity, latency is just the backlog.** At 8 M/s the feed sends the
  window in 0.71 s but the last update arrives about 0.59 s later, so the pipeline kept up with
  5,641,949 / (0.705 + 0.588) = **4.4 M records/s**, matching the unpaced three-thread throughput
  (4.2 M/s). Two separate measurements agree, which is a good check on both.
- **p99 is 65–72 µs whatever the rate from 0.5 to 2 M/s.** That points to the machine stopping,
  not to queueing in the pipeline. If something stops a core for S µs, f times a second, then
  every record that arrives during the stop waits, which is a fraction f × S of all records,
  **independent of the rate**. So about 1% of the time, some stage is stopped for tens of
  microseconds. Candidates, not yet separated:
  - WSL2's hypervisor taking a core away; three spinning cores give it three chances.
  - The engine's slow messages, the phase 4 page-fault hypothesis.
  Test both on bare metal with isolated cores (`isolcpus`, `nohz_full`), and with phase 6's
  pre-touched level arrays.
- **p99.9 and above are WSL2 noise here.** The rounds differ by 3–17 times (1 M/s: 11.4 ms against
  0.65 ms), the ring's own max is 1–18 ms, and the low rates have worse tails than 2 M/s, so the
  tails follow the host, not the load. Read no trend into them.
- **Three threads are slower than one: 4.2 M/s against 5.8 M/s.** The one-thread run also
  encodes and writes the log, so splitting the work should have helped. Leading hypothesis,
  not yet tested: the engine's counter's cache line bounces between cores.
  - Ring 1 is full 4.5 times per message, so the feed spins re-reading the engine's `tail_`.
  - Each re-read pulls that line to the feed's core, and the engine's next `tail_` store must
    take it back: about one one-way hop (~90 ns) on the engine's path.
  - The gap is 237 − 174 = 64 ns per message, about that size.
  Fix to try in phase 6, before/after (rule 10): when the ring is full, let the feed wait for a
  batch of free slots instead of one, or have the engine publish `tail_` every N records.
  Measure the engine's ns per message and the ring-1-full count.
  - Step 3's runs, which wrote the 20 GB log to disk, gave three threads 81.7 s against one
    thread's 86.0 s. There, the disk writes move off the engine's core and that win is bigger.
- **The three-thread throughput is the noisiest number (15% between rounds)**: three spinning
  threads on a shared host. The single-thread number agrees to 1.6%.

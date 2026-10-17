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
- **No consistent winner.** Every difference is within ±6%. Most are inside the round-to-round
  noise; the three slightly beyond it point opposite ways (64 bytes slower on `FastBook`'s 10- and
  100-level sweeps, faster on `TickFastBook`'s 10-level sweep).
- **So the order stays at 48 bytes**: the simpler choice, and a quarter less memory. Even the large
  book's orders (100,000 × 48 bytes, about 5 MB) fit in this CPU's 30 MB shared L3 cache, where a
  straddled line costs little; a book too big for L3, and phase 3's tail-latency numbers, are where
  a difference would show, if anywhere.

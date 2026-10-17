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

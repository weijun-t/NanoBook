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

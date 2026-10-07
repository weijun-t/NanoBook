# NanoBook

[![CI](https://github.com/weijun-t/NanoBook/actions/workflows/ci.yml/badge.svg)](https://github.com/weijun-t/NanoBook/actions/workflows/ci.yml)

An ultra-low-latency limit order book and matching engine in modern C++ (C++23), built as a quant developer portfolio project.

> **Status:** Phase 0 complete (build system, tests, sanitizers, CI). Phase 1 (reference book) in progress. Every number in this README that is not marked *target* was measured. The hardware, OS, compiler and flags are listed next to it.

---

## 1. What this project is (and is not)

NanoBook has two parts that share one book core:

| Component | What it does | Why it matters for quant dev |
|---|---|---|
| **Matching engine** | Accepts orders (limit, market, cancel, modify, IOC, FOK) and matches them by **price-time priority (FIFO)** | Exchange-side logic. Shows that you understand market microstructure. |
| **Feed handler / book builder** | Parses **NASDAQ TotalView-ITCH 5.0** and rebuilds the full-depth (L3) book for every symbol | Trading-side logic. This is what HFT firms actually run, and it gives you **real data** to benchmark against. |

**Out of scope** (until the core is done and measured): networking/kernel bypass, FIX gateway, multiple matching algorithms (pro-rata), persistence.

### UI: no web front-end (yet)
Quant dev interviewers judge the engine, not a UI. A web app (React + WebSocket server) eats weeks and proves nothing about latency. Worse, a naive integration (a server thread reading the book) would break rule 5 (no locks) and rule 6 (no syscalls).

Instead, build the CLI tools, plots and `perf` flame graphs described in **§7 Testing & tooling**. *(Optional, after phase 7 only)* A live view: a **separate process** subscribes to the publisher's output (shared-memory ring or UDP) and renders a TUI (e.g. FTXUI) or a small web page. The engine thread must never know it exists.

A small book with measured latency, proven correctness and a clear README beats a large unmeasured one. Interviewers will ask "how did you measure that?" before they ask about anything else.

---

## 2. Language standard: C++23 (decision and reasoning)

**Decision: C++23 (`-std=c++23`, `CMAKE_CXX_STANDARD 23`), built with GCC (primary) and Clang (secondary) on Linux.** Once phases 1–6 are finished, you may move to C++26, but only if you want reflection for tooling. That is a deliberate, separate commit.

| Option | Verdict | Reason |
|---|---|---|
| C++20 | Too low | It lacks `std::byteswap` (ITCH is big-endian) and `std::expected` (exception-free errors). You would hand-roll both. |
| **C++23** | **Use this** | Full GCC and Clang support. It has every feature that matters for this project, with zero toolchain risk. |
| C++26 | Not yet | No speed benefit. Support is partial, and the useful features (`inplace_vector`, reflection) aren't needed for the core. Debugging a compiler gap wastes time you should spend measuring. |

Facts to keep straight:
- **The language standard does not make your order book faster.** Runtime speed comes from data layout, allocation discipline, branch behaviour and cache behaviour. These work the same way in C++17. The standard you pick is about expressiveness and safety, not nanoseconds.
- C++26 compiler and standard-library support is **still partial and uneven**. GCC is furthest ahead. Upstream Clang lags on some features, such as reflection. **MSVC is not a target for this project.** Check every feature on the cppreference "compiler support" page before you use it. Do not assume.
- HFT firms mostly build with GCC or Clang on Linux. Reviewers will be comfortable with C++20/23 idioms. Heavy use of brand-new features in the core can read as "showing off" if you can't defend each one.

Why not stay on C++17? You would give up concepts, `std::span`, `<bit>`, `consteval`, `[[likely]]`, `std::expected` and `std::unreachable`. These make low-latency code safer and clearer at zero runtime cost.

### Feature whitelist
| Standard | Use freely | Why |
|---|---|---|
| C++20 | concepts / `requires`, `std::span`, `<bit>` (`std::bit_cast`, `std::countr_zero`, …), `consteval`/`constinit`, designated initializers, `[[likely]]`/`[[unlikely]]` (only after measuring), `std::atomic_ref`, three-way comparison | Replace SFINAE and virtual interfaces at compile time. Safe type punning (no `reinterpret_cast` UB when parsing ITCH). Bit tricks for bitmaps of price levels. |
| C++23 | `std::expected` (non-hot-path errors), `std::unreachable`, `std::to_underlying`, `std::byteswap` (ITCH is big-endian!), deducing `this`, `std::print` (cold path only) | Error handling without exceptions. Endianness conversion with no hand-rolled shifts. |
| C++26 (later, optional) | Static reflection for tooling only (enum-to-string, message pretty-printing), never required for the core to build | Talking point only. Isolate it. |

**Banned on the hot path regardless of standard:** `std::function`, `std::shared_ptr`, `std::any`, `std::variant` visitation inside the inner loop (until benchmarked), `<iostream>`, `std::format`/`std::print`, coroutines.

---

## 3. Rules (mandatory)

### 3.1 Hot-path rules
The hot path is everything from "message received" to "book updated / fills emitted".

1. **Zero heap allocations after warm-up.** No `new`, `malloc`, or growth of `vector`/`string`/`unordered_map` on the hot path. Preallocate pools at startup. *Verify it:* run a test with an allocation-counting hook (override global `operator new`) that asserts the count is 0 during replay.
2. **Prices are integers.** Store ticks as `int64_t` (or a strong type wrapping it). **Never `double` for price.** Floating-point equality and rounding will break matching. Quantities are integers too.
3. **No exceptions thrown on the hot path.** Exceptions cost (almost) nothing when not thrown, but a throw costs microseconds. Return status codes or `std::expected`. Decide early whether you build with `-fno-exceptions` and document the decision.
4. **No virtual dispatch on the hot path.** Use templates/concepts, CRTP, or a `switch` on message type. The real cost of virtual calls is lost inlining plus an indirect branch, not "one pointer chase".
5. **No locks in the engine.** The engine is **single-threaded** (the LMAX model). Threads communicate only through **lock-free SPSC ring buffers**.
6. **No syscalls on the hot path.** That means no logging I/O, no `clock_gettime` in tight loops (use `rdtsc` and calibrate), no `std::cout`.
7. **Logging is asynchronous and binary.** The hot path writes a fixed-size record into a ring buffer, and a separate thread formats it.
8. **Every container on the hot path has a fixed, documented capacity.** Decide what happens when it fills (reject, or abort in debug). Undefined growth is a bug.
9. **Cache-line awareness.** Know `sizeof` of every hot struct, and `static_assert` it. Pad shared atomics to 64 bytes (`alignas(64)`). On Intel, adjacent-line prefetch can make 128 bytes the safer choice. `std::hardware_destructive_interference_size` exists, but GCC warns that its value is not ABI-stable (`-Winterference-size`). Understand that warning before you rely on it.
10. **No optimisation without a benchmark before and after.** No `[[likely]]`, `__builtin_expect`, manual prefetch, `inline` forcing or `-O` flag changes unless a measured improvement is committed alongside.

### 3.2 Correctness rules
11. **Deterministic.** The same input stream must produce byte-identical output: no dependence on time, address ordering or hash-iteration order.
12. **Differential testing.** Keep a slow, obviously correct reference book (built on `std::map` and `std::list`). Fuzz random order streams through both and assert identical fills and book state.
13. **ITCH replay validation.** After replaying a full day for a symbol, the book must be internally consistent: no crossed book outside auctions, no negative quantities, every executed or cancelled order ID existed.
14. **Sanitizers in CI:** ASan + UBSan on tests, and TSan on anything multi-threaded. Benchmarks run **without** sanitizers in a separate Release build.
15. **Warnings are errors:** `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`. `clang-tidy` and `clang-format` are enforced in CI.

### 3.3 Measurement rules
16. **Report percentiles, never just the mean:** p50, p90, p99, p99.9, max, plus a histogram (HdrHistogram-style).
17. **Measure per-operation latency with your own `rdtsc` harness.** Google Benchmark is fine for throughput and microbenchmarks, but it reports averages over iterations and does not give tail latency per operation.
18. **Beware coordinated omission** when measuring a pipeline with a producer. Timestamp at the intended send time, not when the consumer got around to it.
19. **Every published number states:** CPU model, core count, OS/kernel, compiler and version, flags, whether the core was isolated, and input dataset.
20. **Benchmark on bare-metal Linux.** WSL2 is fine for development, but its timings are noisy and hardware perf counters are often unavailable. Use native Linux (dual boot, or a cloud bare-metal or dedicated instance) for published numbers. Pin threads (`taskset` / `pthread_setaffinity_np`) and, ideally, isolate cores (`isolcpus`, `nohz_full`). Disable turbo/frequency scaling or report it.

### 3.4 Process rules
21. Commit small. Each commit builds and passes tests.
22. Every performance PR or commit includes a before/after table.
23. Write a design note (`docs/`) **before** each major component, then get it reviewed.
24. You don't keep code you can't explain line by line in an interview.

---

## 4. Fact checks: common misconceptions to avoid

| Belief | Reality |
|---|---|
| "`std::map` is fine for price levels, O(log n)." | Every node is a separate heap allocation, so you get pointer chasing and cache misses. Order flow concentrates near the top of book. A **sorted array or vector of levels with the best price at the end** (cheap insert and erase near the top, linear scan from the top) or a **tick-indexed array** for a bounded price range usually wins by a large margin. See David Gross, CppCon 2024. **Measure both.** |
| "`std::list` for the FIFO queue at a level." | That is one allocation per order. Use an **intrusive doubly linked list** whose nodes live in a preallocated pool, which gives O(1) cancel by handle. |
| "`std::unordered_map<OrderId, Order*>` for lookup." | It is node-based with chaining, so it allocates and chases pointers. Use an **open-addressing flat hash map** with a capacity reserved up front. If IDs are dense or monotonic (ITCH order reference numbers grow), a **direct-indexed vector** may be best. |
| "Big-O decides speed." | At these sizes, constant factors and cache behaviour dominate. One L3 miss (~tens of ns) or one DRAM access (~100 ns) costs more than many instructions. |
| "Multithreading the matching makes it faster." | Matching on one book is inherently sequential. Use threads for **pipeline stages** (feed parsing → engine → publisher) over SPSC queues, not for the match itself. You can shard by symbol later. |
| "`-O3 -ffast-math` makes it faster." | `-ffast-math` affects floating point, which you shouldn't have on the hot path, and it breaks IEEE semantics. Use `-O2`/`-O3 -march=native`, then try **LTO** and **PGO** and measure. Note that `-march=native` binaries are not portable. |
| "`volatile` makes it thread-safe." | It doesn't. Use `std::atomic` with explicit memory orders (`acquire`/`release` for SPSC) and be able to explain why. |
| "Average latency of 50 ns means it's fast." | Tail latency is what matters. A 50 ns mean with a 50 µs p99.9 is a bad engine. |
| "`reinterpret_cast` the ITCH buffer into a struct." | That is strict-aliasing and alignment UB. Use `std::memcpy` or `std::bit_cast` into fields, then `std::byteswap`. Compilers optimise `memcpy` into a single load. |
| "Modify = cancel + add." | Partly true. On most venues, **reducing quantity keeps time priority, while increasing quantity or changing price loses it**. Implement and test both cases explicitly. |
| "Market orders need a price." | They don't. Define the behaviour when the opposite side is empty or too thin (cancel the remainder, as IOC does). |

---

## 5. Architecture sketch (to refine in `docs/design.md`)

```
 ITCH file ──► Parser ──SPSC──► Engine / BookBuilder ──SPSC──► Publisher / Logger
 (mmap)        (thread A)       (thread B, pinned,           (thread C)
                                 single-threaded book)
```

Core data structures (decide and benchmark):
- `Order`: id, side, price (ticks), qty, prev/next indices (intrusive). It is pool-allocated, and the hot fields fit in one cache line.
- `PriceLevel`: price, total qty, head/tail order indices.
- `BookSide`: compare a sorted vector of levels against a tick-indexed array (plus a bitmap to find the next non-empty level via `std::countr_zero`).
- `OrderId → Order` lookup: compare a flat hash map against a direct-indexed vector.
- Use **indices (`uint32_t`) instead of pointers** inside pools. They are smaller, relocatable and cache-friendlier.

---

## 6. Roadmap

| Phase | Deliverable | Exit criteria |
|---|---|---|
| 0. Setup | CMake + Ninja + presets (Debug/ASan, Release, RelWithDebInfo), GCC + Clang, GoogleTest or Catch2, Google Benchmark, clang-format/tidy, GitHub Actions CI on Ubuntu | CI is green with an empty test, and sanitizer builds work |
| 1. Reference book | Naive `std::map` + `std::list` matching engine: limit, market, cancel, modify, IOC, FOK | Unit tests for every order type and edge case (partial fills, self-cross, empty book) |
| 2. Fast book v1 | Pools, intrusive lists, flat lookup, chosen level structure | Differential fuzz against phase 1 passes on 10⁷+ random ops, and the zero-allocation test passes |
| 3. Benchmark harness | `rdtsc` harness, histogram, percentiles, pinned core, `nanobook_bench`, `nanobook_fuzz`, `plot_latency.py` | First published latency table and plot with full environment info |
| 4. ITCH 5.0 | mmap file reader, zero-copy parser, book builder per symbol, `nanobook_replay`, `plot_depth.py` | Full-day replay completes, consistency checks pass, and messages/sec plus per-message latency are reported |
| 5. Pipeline | SPSC ring buffer (your own; explain the memory ordering), 3-stage threads, async binary logger | TSan is clean, and end-to-end latency percentiles are reported |
| 6. Optimise | Data layout experiments, LTO/PGO, `perf stat`/`perf record`, cache-miss analysis | Each change has a before/after table in `docs/perf-log.md` |
| 7. Polish | README results with latency/depth plots, design docs, flame graphs, an "interview notes" section on trade-offs you made | A stranger can build, test and reproduce your numbers in under 10 minutes |

*Target* (sanity check, not a promise): single-threaded add/cancel/execute with the book in cache should land in the **tens to low hundreds of nanoseconds at p50**. If you are in microseconds, something is allocating, locking or making a syscall.

---

## 7. Testing & tooling

All performance tools are **Linux-only**. On WSL2, `perf` is often missing or can't read hardware counters, so publish numbers from native Linux (see rule 20).

### 7.1 Correctness testing
| Tool | Purpose | When |
|---|---|---|
| **GoogleTest** (or Catch2; pick one) | Unit tests for every order type and edge case: partial fills, full sweeps, cancelling the last order at a level, modify that keeps vs loses priority, an empty opposite side | Every commit (CI) |
| **Differential fuzzer** (`nanobook_fuzz`) | Seeded random order streams fed to the reference book and the fast book. Asserts identical fills and book state. Prints the seed on failure so the run is reproducible | CI (short run), nightly/local (10⁷+ ops) |
| **libFuzzer** (Clang `-fsanitize=fuzzer`), optional | Feeds malformed bytes to the ITCH parser. The parser must reject them, never crash | After phase 4 |
| **ASan + UBSan** | Memory errors, UB | Debug preset, every test run |
| **TSan** | Data races in the SPSC queue and pipeline | Every test run once threads exist |
| **Allocation-counting test** | Overrides global `operator new`/`delete` and asserts **0 allocations** during the hot loop | Every commit |
| **Golden replay test** | Replays a small fixed ITCH slice and compares the final book and stats to a checked-in expected output | Every commit |

### 7.2 Benchmarking
| Tool | Purpose | Note |
|---|---|---|
| **Custom `rdtsc` harness** | Per-operation latency, written to a histogram → p50/p90/p99/p99.9/max | **This is the source of every published latency number.** Calibrate TSC→ns at startup, check `constant_tsc`/`nonstop_tsc` in `/proc/cpuinfo`, and use `rdtscp` or `lfence` for ordering. Subtract the measured timer overhead. |
| **Google Benchmark** | Microbenchmarks to compare data-structure alternatives (map vs vector vs array levels, hash maps) | Gives throughput and averages only, not tail latency |
| **Throughput replay** | Full-day ITCH replay, reporting messages/sec | Load the file into memory first (mmap + pre-fault) so you measure the book, not the disk |

### 7.3 Profiling with `perf`
| Command | What it tells you |
|---|---|
| `perf stat -e cycles,instructions,cache-misses,branch-misses,L1-dcache-load-misses ./nanobook_bench` | IPC, cache-miss and branch-miss rates. **Run this first for every optimisation.** |
| `perf record -g --call-graph dwarf ./nanobook_replay …` then `perf report` | Where the time goes, by function and line. Build with `RelWithDebInfo` (`-O2 -g`, plus `-fno-omit-frame-pointer`). |
| `perf script` → FlameGraph (Brendan Gregg's scripts or `inferno`) | A **flame graph SVG** for the README |
| `perf c2c` | Detects false sharing between threads (SPSC head/tail, logger) |
| `perf annotate` / Compiler Explorer | The hot loop's assembly. Check that what you expected to inline actually did, and look for unexpected calls. |
| Optional: `valgrind --tool=cachegrind`, `heaptrack` | Deterministic cache simulation, and confirmation of allocation counts |

### 7.4 CLI tools (in `tools/`; they are part of the deliverable)
| Binary | Does | Output |
|---|---|---|
| `nanobook_replay <itch_file> [--symbol AAPL] [--depth 10]` | Replays ITCH, rebuilds the book, prints top N levels and stats (messages, orders, trades, msgs/sec) | Human-readable table, plus `--snapshot-csv` for depth snapshots |
| `nanobook_bench [--ops N] [--seed S] [--core C] [--csv out.csv]` | Runs the `rdtsc` harness on a pinned core for each operation type (add/cancel/modify/execute/match) | Percentile table printed with environment info. Raw samples or histogram go to CSV. |
| `nanobook_fuzz [--ops N] [--seed S]` | Differential test of the reference book against the fast book | PASS, or FAIL with a seed and the first diverging operation |

Rules for the tools: they live **outside** the engine library and only use its public API. All output (printing, CSV) happens after the timed region, never inside it.

### 7.5 Plots (`scripts/`, Python + pandas + matplotlib)
| Script | Plot | Shows |
|---|---|---|
| `plot_latency.py` | Histogram (log-scale x axis) plus a percentile curve (p50 → p99.99) for each operation | Tail latency at a glance, which is the headline README image |
| `plot_compare.py` | Before/after percentile curves for two CSVs | Proof that an optimisation helped (used in `docs/perf-log.md`) |
| `plot_depth.py` | Book depth (cumulative qty by price, bid vs ask) from a snapshot CSV | That the book reconstructed from real ITCH data looks right |
| Flame graph SVG | From §7.3 | Where the time goes |

Store generated images in `docs/img/` and embed them in the README Results section, each with the environment it came from. The plotting scripts read only CSV and never touch the engine.

---

## 8. Learning path (C++17 → C++23)

Learn these **as you need them**, in this order:
1. C++20: concepts, `std::span`, `<bit>`, `consteval`/`constinit`, designated initializers, `std::atomic_ref`, attributes `[[likely]]`/`[[no_unique_address]]`
2. C++23: `std::expected`, `std::byteswap`, `std::unreachable`, `std::to_underlying`, deducing `this`
3. Memory model: `memory_order_acquire`/`release`/`relaxed`. Be able to explain why an SPSC queue needs only acquire/release.
4. C++26 (only after phase 6): reflection, for tooling only

---

## 9. References
- David Gross, *When Nanoseconds Matter: Ultrafast Trading Systems in C++*, CppCon 2024 (order book design specifically)
- Carl Cook, *When a Microsecond Is an Eternity: High Performance Trading Systems in C++*, CppCon 2017
- Bilokon & Gunduz, *C++ Design Patterns for Low-Latency Applications Including High-Frequency Trading* (arXiv, 2023)
- Sourav Ghosh, *Building Low Latency Applications with C++* (Packt, 2023)
- Ulrich Drepper, *What Every Programmer Should Know About Memory*
- Denis Bakhvalov, *Performance Analysis and Tuning on Modern CPUs*
- Agner Fog, optimisation manuals and instruction tables
- Martin Thompson et al., *LMAX Disruptor* technical paper
- NASDAQ TotalView-ITCH 5.0 specification and public sample files (emi.nasdaq.com/ITCH)
- Compiler Explorer (godbolt.org): check your hot-path assembly
- cppreference compiler-support table: check before using any newer feature

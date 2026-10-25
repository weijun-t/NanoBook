# NanoBook

[![CI](https://github.com/weijun-t/NanoBook/actions/workflows/ci.yml/badge.svg)](https://github.com/weijun-t/NanoBook/actions/workflows/ci.yml)

A limit order book and matching engine in C++23, built to be fast and to prove it. It rebuilds the
full book of every Nasdaq stock from a real trading day (368 million ITCH 5.0 messages) at **68 ns
per message on one core**, with **no heap allocation and no page fault while trading**. After
every one of the day's 363 million order messages, it matches a slow, obviously correct reference
book. Every number below was measured on bare-metal Linux, and you can rerun the checks in about
a minute.

## Results

Bare metal, one session, one commit (`f895be6`). Two rounds each; a range is round 1 to round 2.

| What | p50 | p99 | p99.99 | Throughput |
|---|---|---|---|---|
| Add an order (synthetic, 1M samples each) | 19.8–21.9 ns | 30.6–33.9 ns | 43.0–44.2 ns | |
| Cancel an order | 44.6–45.1 ns | 61.6–62.0 ns | 92.2–99.2 ns | |
| Modify an order | 50.4–50.8 ns | 76.9–77.3 ns | 98.0–98.4 ns | |
| Match (one order fully fills one resting order) | 16.9–17.4 ns | 19.4–19.8 ns | 28.1–32.2 ns | |
| **The real Nasdaq day**, every 8th order message timed | 62.4–62.8 ns | 295.6 ns | 607.6–610.5 ns | **67.7–68.0 ns per message** (14.7 M/s) |
| **Three-thread pipeline**, feed to publisher, at 1 / 2 / 4 M messages/s | 383–390 / 375–379 / 381–384 ns | 631–638 / 630–634 / 680–686 ns | 0.96–1.53 µs | |

- **Machine:** Intel Core i7-14650HX, Ubuntu 26.04.1 LTS (live USB, not a VM), Linux
  7.0.0-30-generic with `isolcpus=8-13 nohz_full=8-13`, pinned to core 8 (pipeline: 8, 10, 12),
  `performance` governor, turbo on. GCC 14.3.0, `-std=c++23 -O3`, no `-march=native`, LTO or PGO.
- **Data:** Nasdaq TotalView-ITCH 5.0, 30 January 2019 (`scripts/fetch_itch.sh`). Times are in
  ns with the timer's own overhead subtracted. Raw output: `docs/data/bare_metal/release/`.
- **Phase 6's optimisations, in total:** 86.4 → 67.9 ns per message on the real day, p50 90 → 62
  ns. One cost: `match` is 8–17% slower in the synthetic micro-benchmarks since the 8-byte ID
  entries (E9b); the real day is 9–10% faster, and its matches are 13–21% faster at p50
  (`docs/perf-log.md`, 2026-10-25).

![Latency per operation at each percentile: reference book, fast book, tick-array book](docs/img/release_phase3_percentiles.png)

![Pipeline latency against feed rate](docs/img/release_phase5_latency.png)

![What phase 6 bought on the real day](docs/img/release_phase6_total.png)

**Where the time goes** (the real day, `perf` with LBR call stacks;
[open the SVG](docs/img/release_flamegraph.svg) to zoom). The graph covers the whole program: the wide tower under `BookBuilder` is the timed
replay (48.5% of samples), and the one under `DayCounter` is the untimed pre-scan that sizes the
books (41%). Inside the timed replay, the order-ID table takes 60% of the time and the price
levels 24%.

![Flame graph of the real-day replay](docs/img/release_flamegraph.svg)

The rebuilt book looks like a market (AAPL at the close):

![AAPL depth at the close](docs/img/phase4_depth_aapl_close.png)

## Quickstart

Needs Linux on x86-64, GCC 14, CMake 3.25 or newer, and Ninja. On Ubuntu 24.04:
`sudo apt-get install -y cmake ninja-build g++-14`.

```bash
git clone https://github.com/weijun-t/NanoBook.git
cd NanoBook
scripts/reproduce.sh        # build, 790 tests, 30M fuzzed operations, the latency table
```

It takes **55 s** from a fresh clone on the machine above, including the GoogleTest and Google
Benchmark downloads. It prints your
latency next to the published numbers. On a laptop, a VM or a busy machine yours will be slower,
and the tools say so (a `NOT FOR PUBLISHING` banner on a VM). Output goes to `repro/`.

The full level adds the real day and the pipeline. It needs the day's file: a 4.76 GB download
(about 6 minutes at 100 Mbit/s) that unpacks to 11.2 GB.

```bash
scripts/fetch_itch.sh
scripts/reproduce.sh --itch data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50
```

The tools, one at a time (built in `build/gcc-release/`):

| Tool | Does |
|---|---|
| `nanobook_replay FILE --throughput --latency --core C` | Replays the day: messages per second, latency per message kind |
| `nanobook_bench --scenario match --core C` | Per-operation latency of the synthetic scenarios, percentiles and histogram |
| `nanobook_fuzz --ops N --seeds S` | Random orders into the fast book and the reference book; PASS, or the first difference |
| `nanobook_pipeline FILE --cores A,B,C --latency --rate R` | The three-thread pipeline, paced at R messages a second |

## How it works

```
 ITCH file ──► Parser ──SPSC ring──► Engine: one book per stock ──SPSC ring──► Publisher / log
 (1 GiB windows) (core A)            (core B, single-threaded)                (core C)
```

- **Orders** live in one array of fixed slots made at start-up. Each price level's queue is a
  doubly linked list through the slots, by `uint32_t` index, not pointer.
- **Price levels:** one sorted array per side, best price at the end. A lookup checks the 4
  levels nearest the best price, then binary-searches.
- **Order ID → slot:** an open-addressing hash table (linear probing, 8-byte entries).
- **Memory:** everything is sized once from a pre-scan of the day and placed on 2 MiB huge pages,
  touched at start-up. That's why there are 0 allocations and 0 page faults while trading.
- **Threads** pass records through NanoBook's own lock-free single-producer, single-consumer
  rings (acquire/release, counters 128 bytes apart). The book itself never sees a second thread.

The designs, with every alternative weighed: `docs/design.md` (start here).

## How it's tested

- **An answer key:** a reference book on `std::map` and `std::list`, simple enough to check by
  eye. The fast book must produce the same events and the same book: on 162 million random
  operations (phase 2), 30 million more in every `reproduce.sh` run, and after every one of the
  real day's 363,118,215 order messages (0 mismatches).
- **790 GoogleTest tests**, run by CI in 6 builds: GCC and Clang, each with Debug + ASan + UBSan,
  Release, and ThreadSanitizer. CI also checks `clang-format` and `clang-tidy`, and runs
  libFuzzer on the ITCH parser for 60 s.
- **Zero allocations:** 19 of those tests replace `operator new` and fail if the fast book
  allocates while trading. (The sanitizer builds leave them out, as ASan and TSan replace
  `operator new` themselves, so those builds run 771.)
- **The engine is built with `-fno-exceptions`**, and every build treats warnings as errors.

## Trade-offs

The full list, with what was tried instead and the evidence: `docs/interview-notes.md`.

- **Sorted array of price levels, not a tick-indexed array.** The tick array was up to 3.9× faster
  on cancels, but sizing one for every stock's real price range would need 489,000 GB.
- **8-byte ID-table entries.** The table is over half the replay's time, so twice as many entries
  per cache line made the day 7% faster. The cost: one extra read to confirm a match (above).
- **Three threads are slower than one** (0.78× unpaced). The book is 96% of the work, and every
  hand-over moves cache lines between cores. A pipeline pays only when it takes slow work, like
  disk writes, off the engine.
- **Huge pages, touched at start-up:** 7% faster on the day, and no page fault while trading.
- **A moving window of price slots near the best price** was 15% faster on average but its p99.99
  was 2.5–6× worse. It stays a tested option (`--book window`), not the default.
- **Published only from bare metal.** On WSL2 the pipeline's p99 was 65–72 µs; on isolated cores
  it's 0.65 µs. That tail was the virtual machine.

## Read more

| Document | What's in it |
|---|---|
| `docs/plan.md` | How the project was run: scope, the rules (hot path, correctness, measurement), the roadmap |
| `docs/final-report.md` | The whole story in plain words: every test, why, what it told us, so what; the conclusion |
| `docs/interview-notes.md` | Each decision: chose, instead of, because, would revisit if; likely questions |
| `docs/design.md` | The reference book and the engine's interface (phase 1) |
| `docs/fast-book-design.md` | The fast book's data structures (phase 2) |
| `docs/latency-harness-design.md` | The `rdtsc` harness and how to read its numbers (phase 3) |
| `docs/itch-design.md` | The ITCH 5.0 parser, the book builder, the real-day replay (phase 4) |
| `docs/pipeline-design.md` | The rings and the three-thread pipeline (phase 5) |
| `docs/optimise-design.md` | The 11 optimisation experiments and their rules (phase 6) |
| `docs/polish-design.md` | This front page, reproducing, the flame graph (phase 7) |
| `docs/phase1-report.md` … `docs/phase6-report.md` | What each phase built, measured and learned |
| `docs/bare-metal-report.md` | The bare-metal sessions: phases 3–6's published numbers |
| `docs/perf-log.md` | Every performance number, with its machine and commit, in date order |

## Status

| Phase | |
|---|---|
| 0. Setup | Done: CMake presets, GCC and Clang, sanitizers, CI |
| 1. Reference book | Done: limit, market, cancel, modify, IOC, FOK, by price-time priority |
| 2. Fast book | Done: 0 allocations while trading, equal to the reference on 162M random operations |
| 3. Latency harness | Done: `rdtsc` per operation, percentiles, published from bare metal |
| 4. ITCH 5.0 replay | Done: the real day, every check passing, equal to the reference after every message |
| 5. Pipeline | Done: three pinned threads over SPSC rings, TSan clean, log identical to one thread |
| 6. Optimise | Done: 11 experiments, 4 kept, each with a before/after table, re-checked on bare metal |
| 7. Polish | Done: this front page, one-command reproduction (55 s), flame graph, interview notes, final report |

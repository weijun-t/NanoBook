# Phase 3 design: the latency harness

> **Status:** approved (2026-10-18). Changes from here on are made by an edit plus a note in the
> commit message. Decided (§13): raw samples, subtract the timer overhead's minimum, 1,000,000
> samples per scenario, the fuzzer's generator for the mixed stream, dual-boot Ubuntu for the
> published run. Items marked **[MEASURE]** are decided by a measurement in the step named.
> Edits (2026-10-18, step 3a): the `other` tag (§5.3); scenarios moved into
> `support/latency_scenarios.hpp` so tests can check them (§11); step 3 split into 3a and 3b (§12).
> Step 3b: the CSV output is two files, summary and histogram (§9).
> **Phase 3 complete (2026-10-24):** step 6, the published run, on bare metal
> (`docs/bare-metal-report.md`); re-run at the release commit on 2026-10-25
> (`docs/data/bare_metal/release/phase3/`, the README's numbers).

Phase 2 proved the fast book correct and measured its **average** speed with Google Benchmark.
Phase 3 builds the tool that measures **every single operation**, so the report can show the
typical case *and* the slow ones (plan rules 16, 17). It changes nothing in the engine: the
harness is a tool outside the library that only uses the books' public API (plan §7.4).

---

## 1. Goal and exit criteria

**Goal:** a command-line tool, `nanobook_bench`, that times each operation one by one with the
CPU's time-stamp counter, and reports the distribution: p50, p90, p99, p99.9, p99.99 and max, plus
a histogram, with the full environment printed next to the numbers (rule 19).

**Phase 3 is done when** (README roadmap):
1. The harness is **tested**: its maths on known data, its timer against the system clock, and a
   planted slow operation it must catch (§10).
2. A **first published latency table and plot**, with full environment information, from
   **bare-metal Linux** (rule 20).

Criterion 2 needs the dual-boot Ubuntu. Everything else is built and tested on WSL2 first, and
WSL2 output carries a "not for publishing" banner (§8).

---

## 2. Why averages aren't enough

Google Benchmark times a batch of 10,000 operations and divides. That gives the **mean**, which
hides the slow cases. A worked example with 10 operations (in ns, sorted):

```
9   10   10   10   11   11   12   12   15   480
                                              └── one deep insert, or an interrupt
mean  = 580 / 10 = 58 ns      ← describes no operation that actually happened
p50   = 11 ns                 ← the typical operation
p90   = 15 ns
max   = 480 ns                ← the one that matters to a trader waiting on it
```

In trading, the slow operation is a missed trade, so the tail is the headline number. plan §4:
"A 50 ns mean with a 50 µs p99.9 is a bad engine."

The sorted array's known weakness (fast-book design §6.1, deep inserts) is exactly this: rare,
slow, and invisible in an average. Phase 3 is where it finally gets measured.

---

## 3. The timer: the time-stamp counter

### 3.1 What it is
Every x86 CPU has a 64-bit counter that goes up at a fixed rate from power-on, like a stopwatch
that never stops. The `rdtsc` instruction reads it in a few dozen CPU cycles, with no system call.
On this CPU the counter is safe to use as a clock (checked in `/proc/cpuinfo`):

| Flag | Meaning | This laptop |
|---|---|---|
| `constant_tsc` | Ticks at a fixed rate, even when the CPU speeds up or slows down | ✓ |
| `nonstop_tsc` | Keeps ticking in power-saving states | ✓ |
| `rdtscp` | The `rdtscp` instruction exists (§3.3) | ✓ |
| `tsc_known_freq` | The kernel knows the tick rate from the CPU itself | ✓ |

The harness checks `constant_tsc` and `nonstop_tsc` at start-up and refuses to run without them.

**Ticks are not CPU cycles.** The counter ticks at a fixed reference rate, while the CPU's real
clock changes with turbo. So the harness reports **nanoseconds**, never "cycles".

### 3.2 Why not `std::chrono`
`std::chrono::steady_clock` calls `clock_gettime`. On Linux that usually avoids a real system call
(the vDSO), but it does more work than one instruction, and plan rule 6 bans it in tight loops.
It is used once, at start-up, to calibrate the counter (§4).

### 3.3 Reading it safely: two kinds of reordering
A modern CPU runs instructions **out of order**: it starts later instructions while earlier ones
are still in flight. Without care, the start stamp could be read *after* the operation began, or
the end stamp *before* it finished. The compiler can also move code. Both are handled:

```
start():  rdtscp            waits until every earlier instruction has finished
          lfence            stops later instructions (the operation) from starting early
          ── book.handle(input, out) ──
stop():   rdtscp            waits until the operation has finished
          lfence            stops the next code from starting before the stamp is read
```

- **CPU reordering:** Intel's manual describes `rdtscp` as waiting for all previous instructions,
  and `lfence` after it as stopping later ones from starting early. That gives the pattern above.
- **Why not the textbook `cpuid` fence:** Intel's old white paper used `cpuid` as the fence, but
  inside a virtual machine (WSL2) `cpuid` exits to the hypervisor and costs far more than the
  operation being timed. `lfence` does the job without that.
- **Compiler reordering:** each stamp is wrapped in `asm volatile("" ::: "memory")`, which tells
  the compiler "memory may have changed here", so it can't move the book's reads and writes across
  the stamp. **Verified by reading the assembly** (Compiler Explorer, or `objdump -d` of the
  binary) in step 1: the call to `handle` must sit between the two `rdtscp`.
- **A free check:** `rdtscp` also returns a number Linux sets to the **core** it ran on. If the
  start and stop stamps came from different cores, the thread moved mid-operation and the sample
  is meaningless. Such samples are dropped and **counted**; with the thread pinned (§7) the count
  must be 0, which is itself a check on the pinning.

In C++ these are compiler intrinsics from `<x86intrin.h>`: `__rdtscp(&core)` and `_mm_lfence()`,
both in GCC and Clang. The harness is x86-64 only and says so with an `#error` elsewhere.

### 3.4 What the timer costs, and what that means
Reading the counter twice with fences is not free. **[MEASURE, step 1]** The harness times an
empty region many times at start-up and reports that **timer overhead** as its own distribution.
The fast book's simplest operations take about 10–13 ns on average (phase 2 report §7.4), so the
overhead may be of the same size. Two consequences, both stated in every report:
- The overhead's **minimum** (its fixed part) is **subtracted** from each sample, and the full
  overhead distribution is printed (§13 Q2).
- The fences stop the CPU from overlapping one operation with the next, which it normally does.
  So per-operation latency is **slower than throughput / N**. Both are reported (§6.3).

---

## 4. Turning ticks into nanoseconds

`ns = ticks × 1e9 / ticks_per_second`. The tick rate is found at start-up (cold path, so system
calls are fine there):

1. Read the counter and `clock_gettime(CLOCK_MONOTONIC_RAW)` together, wait about 200 ms, read both
   again. `rate = Δticks / Δseconds`. (`_RAW`: not adjusted by network time sync while measuring.)
2. Do it 5 times and use the **median**, and print the spread. A spread above 0.1% means the
   clock itself is unreliable on this machine, and the report says so.

Worked example: if the rate comes out at 2.5 billion ticks per second, a sample of 50 ticks is
50 × 1e9 / 2.5e9 = **20 ns**.

At the end of the run the calibration is repeated, so the report shows whether the rate drifted
during the run. The conversion uses `double`: fine, it's reporting, not prices (rule 2).

---

## 5. Recording the samples

### 5.1 Raw samples, kept in full
For each timed operation the harness stores:

| Array | Type | Size | Why |
|---|---|---|---|
| `ticks[i]` | `uint32_t` | 4 bytes | The duration. 32 bits hold up to ~1.7 s at 2.5 GHz; longer is clamped and counted |
| `tag[i]` | `uint8_t` | 1 byte | What the operation was (§5.3) |

Both arrays are **allocated and touched before timing starts**, so the first write to each memory
page (a page fault, microseconds) never lands inside a sample. 1 million samples = 5 MB.

**Decided (§13 Q1): every raw sample is kept**, not only a histogram. Raw samples give exact
percentiles, let the plot script re-bucket later, and keep the **order** the operations happened
in, which shows whether slow ones repeat at a fixed interval (a sign of interrupts, not the book).
A histogram would use fixed memory (about 20 KB) but round every value to its bucket and lose the
order. Writing 5 bytes per operation into a sequential array is the only work the recorder adds
inside the loop, and the CPU's prefetcher handles sequential writes well. If phase 4's full-day
replay (hundreds of millions of messages, 1–2 GB of raw samples) needs it, a histogram-only mode
can be added then, reusing §5.4's bucketing.

### 5.2 Percentiles: nearest rank
After the run, the samples are sorted (cold path). The p-th percentile is the sample at position
`ceil(p/100 × N) − 1` in sorted order (the "nearest-rank" definition, so every reported value is a
real measured sample, never an interpolation). Worked example with the 10 samples of §2:
p90 → `ceil(0.9 × 10) − 1 = 8` → the 9th smallest → **15 ns**.

**How many samples a percentile needs:** p99.9 with 1,000,000 samples rests on the slowest 1,000;
p99.99 on the slowest 100. So **1,000,000 samples per scenario** by default (§13 Q3), and p99.99
is only printed when N ≥ 1,000,000.

### 5.3 Tags: one stream, many kinds of operation
In a mixed stream, an add that rests and an add that sweeps 10 levels are different operations.
After each `stop()` (outside the timed region) the harness looks at the events the book produced
and tags the sample:

| Tag | Rule |
|---|---|
| `add` | A New order that rested without trading |
| `match` | Any input that produced at least one Trade |
| `cancel` | A Cancel that succeeded |
| `modify` | A Modify that succeeded |
| `reject` | Anything Rejected (invalid input, too late, unknown order) |
| `other` | A new order that neither traded nor rested: an IOC or FOK that found nothing (added in step 3) |

The report then gives percentiles **per tag** as well as for the whole stream. Rejects are kept
apart because they're cheap and would make the typical operation look faster than it is.

### 5.4 Histogram (for the plot and CSV)
Built from the sorted samples after the run, so its cost doesn't matter. HdrHistogram-style
buckets: 1 ns wide up to 1,023 ns, then each doubling (1,024–2,047, 2,048–4,095, …) split into 64
equal buckets, so no bucket is more than about 1.6% wide relative to its values. This keeps tails
of 100 µs readable without millions of rows.

---

## 6. What gets measured: scenarios

### 6.1 The loop
Every scenario runs the same loop, on a book built untimed beforehand:

```
warm-up: replay W inputs untimed                    (caches, branch predictor, page faults)
for i in 0 .. N-1:
    t0 = start()
    book.handle(input[i], out)                      ← the only thing timed
    t1 = stop()
    ticks[i] = t1 - t0            (if both stamps are on the same core)
    tag[i]   = classify(out)      ← untimed
    out.clear()
```

The inputs are **generated before timing** into an array, so building an order is never timed.
Scenarios that use up their book (cancel removes every order) rebuild it untimed between batches,
as the phase 2 benchmarks did.

### 6.2 The scenarios
Each runs on `ReferenceBook`, `FastBook` and `TickFastBook` (`--book`).

| Scenario | Book before timing | One timed operation | Why |
|---|---|---|---|
| `add` | Empty; adds spread over 100 prices | A limit order that rests | The most common operation |
| `cancel` | 10,000 resting orders on 100 prices | Cancel one, in shuffled order | Phase 1's slowest operation |
| `modify` | 10,000 resting orders | Half reduce (keep priority), half increase (lose it) | Not covered by phase 2's benchmarks |
| `match` | 10,000 sells of 1 at one price | A buy of 1 that trades | Matching alone |
| `sweep10` | One order on each of 10 levels | One order that empties all 10 | Cost per level emptied |
| `large_cancel` / `large_add` | 100,000 orders on 10,000 levels | Cancel / add at an existing level | Bigger than L1 and L2 |
| `deep_insert` | 5,000 levels | An add that creates a **new level at a random depth** | The sorted array's worst case (§2) |
| `mixed` | The fuzzer's steered stream, after a 200,000-input warm-up | Every input, tagged (§5.3) | The closest to real flow before phase 4 |

The `mixed` stream reuses the fuzzer's `Generator` (`support/differential_fuzz.hpp`), which steers
cancels at orders that exist by looking at a reference book. The stream is generated once with a
`ReferenceBook`, then replayed into whichever book is measured. All three books behave
identically, so the stream is valid for each, and **every book sees exactly the same inputs**.
This stream includes deliberately invalid inputs (the fuzzer needs them); they're tagged `reject`
and reported apart (§13 Q4).

### 6.3 Throughput mode
`--throughput` runs the same inputs with **one** start/stop around the whole run instead of one
per operation. That gives operations per second and the mean, with the CPU free to overlap
operations as it does in real use. Comparing it with the per-operation mean shows how much the
fences themselves cost. It should roughly agree with Google Benchmark on the same scenario; a
large disagreement means one of the two tools is wrong.

### 6.4 Not coordinated omission (yet)
plan rule 18 warns about coordinated omission: in a pipeline, if the engine falls behind, the
orders queuing up wait too, and timing only the engine hides that wait. Phase 3 replays from an
array with nothing waiting, so it measures **service time** (how long the book takes per input),
which is what's wanted here. Phase 5's pipeline measures response time, and that's where rule 18
applies.

---

## 7. Making the machine quiet

On bare metal, before a published run (rule 20):

| Setting | How | Why |
|---|---|---|
| Pin to one core | `--core C` (`sched_setaffinity` at start-up, checked with `sched_getcpu`) | The thread never moves; §3.3's core check confirms it |
| A **performance core** | Pick C from `lscpu --extended` | The i7-14650HX mixes fast P-cores and slower E-cores |
| Isolate that core | Kernel options `isolcpus=C nohz_full=C` | Nothing else is scheduled there, and the timer tick mostly stops |
| Fixed frequency | `performance` governor; turbo off, or on and stated | Turbo changes speed with temperature |
| Nothing else running | Close everything | Same lesson as phase 2's benchmarks |

Outliers are **never thrown away**: an interrupt landing mid-operation is a real slow operation on
this machine. The report shows the max and the p99.99, and the environment says whether the core
was isolated, so a reader can judge them.

---

## 8. The environment, printed with every result (rule 19)

The harness collects these itself, at start-up (cold path):

| Item | Source |
|---|---|
| CPU model, cores | `/proc/cpuinfo` |
| Kernel, OS | `uname`, `/etc/os-release` |
| **WSL2?** | `/proc/sys/kernel/osrelease` contains "microsoft" → a **NOT FOR PUBLISHING** banner on every output |
| Compiler and version | `__VERSION__`, compiled in |
| Build flags, build type | Passed in by CMake |
| Git commit | Passed in by CMake, regenerated on every build (and "-dirty" if there were uncommitted changes → a **NOT FOR PUBLISHING** banner too) |
| Core used, isolated cores | `--core`, `/sys/devices/system/cpu/isolated` |
| Governor, turbo | `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`, `.../intel_pstate/no_turbo` (printed as "unknown" if missing, as on WSL2) |
| Counter rate, its spread, drift during the run | §4 |
| Timer overhead | §3.4 |
| Samples dropped (core moved), clamped (too long) | §3.3, §5.1 |
| Seed, N, scenario, book | Command line |

---

## 9. Output

**Terminal:** the environment block, then one table per scenario and book:

```
scenario=cancel book=FastBook  N=1,000,000  overhead=… ns (subtracted)
tag      count      min    p50    p90    p99    p99.9   p99.99   max
cancel   1000000    …      …      …      …      …       …        …
```

**CSV** (`--csv out.csv`): two files, each starting with the environment as `#` comment lines.
`out.csv` has one row per (mode, scenario, book, tag) with every percentile; `out_histogram.csv`
has the histogram buckets (two kinds of rows in one file would be awkward to load). `--raw
FILE.csv` writes every sample in the order it was taken (large; for one-off investigation, such
as whether slow samples repeat at a fixed interval). All printing and file writing happens
**after** the timed loop (plan §7.4).

**Plot:** `scripts/plot_latency.py` reads only the CSV (plan §7.5):
1. a histogram per scenario with a log-scale time axis, the three books overlaid;
2. a **percentile curve**: x axis p50 → p90 → p99 → p99.9 → p99.99 (spaced by "number of nines"),
   y axis latency, one line per book. This is the headline README image.

Same style and palette as `scripts/plot_phase2.py`.

---

## 10. Proving the harness right

A wrong harness gives confident wrong numbers, so it gets the same treatment as the fast book.

| Check | What it proves | How |
|---|---|---|
| Percentile maths | Nearest rank on known data, including N = 1, ties, p100 = max | Unit tests |
| Histogram buckets | Every value lands in the right bucket, edges included; bucket counts add up to N | Unit tests |
| Tick → ns | The conversion and its rounding | Unit tests with a fixed rate |
| Calibration | The counter agrees with the system clock over a long run (within 0.1%) | Start/end calibration, printed (§4) |
| **Planted slow operation** | The tail is really seen: a fake book that spins for about 1 µs on every 1,000th input must show p99.9 ≥ 1 µs and p50 unchanged | Test, the harness's "teeth" (like phase 2's planted bugs) |
| No allocation in the timed loop | The harness itself obeys rule 1 | The counting `operator new` (phase 2), asserting 0 during the loop |
| The stamps surround the call | No compiler reordering (§3.3) | Read the assembly once, record it in the step's commit |
| Agreement with Google Benchmark | Throughput mode's mean ≈ Google Benchmark's mean on the same scenario, same session | Step 5 |

CI builds `nanobook_bench` and runs a smoke test (a few thousand operations per scenario) to show
it works. CI numbers are never used (rule 20).

---

## 11. Files

```
support/
  tsc.hpp                  start()/stop(), calibration, overhead measurement
  latency_recorder.hpp     sample arrays, tags, percentiles, histogram, the timing loop
  latency_scenarios.hpp    §6's scenarios and tagging (a header, so tests can check them)
  environment.hpp          collects §8's environment (cold path)
tools/
  nanobook_bench.cpp       command line, output
tests/
  test_latency.cpp         §10's unit tests and the planted slow operation
  test_latency_scenarios.cpp  each scenario times what it claims; every book sees the same inputs
scripts/
  plot_latency.py          histogram and percentile curve from the CSV
```

All of it is tool code outside the engine library: it may allocate and print freely **outside**
the timed loop. The engine library gains nothing and changes nothing.

---

## 12. Plan, one commit per step

| # | Step | Done when |
|---|---|---|
| 0 | This design note | Reviewed and committed (rule 23) |
| 1 | Timer (`tsc.hpp`): stamps, calibration, overhead, TSC-flag check | Unit tests pass; overhead and rate printed on WSL2; assembly checked |
| 2 | Recorder (`latency_recorder.hpp`): samples, tags, percentiles, histogram | Unit tests pass, including the planted slow operation |
| 3a | `nanobook_bench`: scenarios on all three books, tagging, terminal table | Every scenario times only what it claims (tests); 0 allocations while timing a fast book; CI smoke test green |
| 3b | Environment block (§8), CSV output, throughput mode | Environment printed and in the CSV; throughput mode runs |
| 4 | `plot_latency.py` | Both plots drawn from a WSL2 CSV |
| 5 | First WSL2 run, recorded in `docs/perf-log.md` (not published) | Throughput agrees with Google Benchmark; the deep-insert tail measured |
| 6 | Bare metal: dual-boot Ubuntu, quiet the machine (§7), run, publish the table and plot in the README | Exit criterion 2; phase 3 report |

Steps 1–5 need only WSL2. Step 6 needs the USB stick.

---

## 13. Decisions (approved 2026-10-18)

| # | Question | Decision | Why |
|---|---|---|---|
| Q1 | Keep every raw sample, or only a histogram? (§5.1) | **Raw samples** | Exact percentiles; 5 MB per million samples; the plot can re-bucket later |
| Q2 | Subtract the timer overhead? (§3.4) | **Subtract its minimum, print the full overhead distribution** | Removes the fixed cost without making any sample negative; readers see the resolution limit |
| Q3 | Samples per scenario (§5.2) | **1,000,000** default, `--ops` to change | p99.99 rests on 100 samples; a scenario takes seconds |
| Q4 | `mixed` stream from the fuzzer's generator, invalid inputs included and tagged apart? (§6.2) | **Yes** for phase 3 | Already tested and seeded; real flow arrives in phase 4 |
| Q5 | Bare-metal route (§1) | **Dual-boot Ubuntu from a USB stick** | Discussed: cheapest, same laptop, so WSL2 and bare-metal numbers can be compared |

## 14. Out of scope for phase 3

| Idea | Where it belongs |
|---|---|
| `perf stat` / `perf record`, cache-miss counts, flame graphs | Phase 6 (and they need bare metal: WSL2 rarely exposes the hardware counters) |
| Optimising anything the numbers point at (e.g. the deep insert) | Phase 6, with before/after numbers (rule 10) |
| Response time under load, coordinated omission | Phase 5 (§6.4) |
| Real order flow | Phase 4 (ITCH) |
| `plot_compare.py` (before/after percentile curves) | Phase 6, when there is a before and an after |

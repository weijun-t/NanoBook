# Phase 6 design: optimise, one measured change at a time

> **Status:** approved (2026-10-21): every decision in §8 as proposed, plus a re-check of every
> kept change on bare metal (§5, B5). Changes from here on are made by an edit plus a note in the
> commit message.

Phases 1–5 built a correct order book, a way to time it, a real Nasdaq day to feed it, and a
three-thread pipeline around it. Phase 5 left measured **symptoms**, such as a slow tail, a pipeline
slower than one thread, and a book 2.5–5× slower on real data than in cache. Phase 6 turns
them into **experiments**. The README's exit criterion: **every change has a before/after table in
`docs/perf-log.md`** (rules 10 and 22).

---

## 0. Background in plain words

### 0.1 What "optimise" means here
Each change is an **experiment** with four parts, written down before any code:
1. **Symptom:** something already measured (a number in the perf log), not a hunch.
2. **Hypothesis:** what causes it, and what number should change if we're right.
3. **Change:** the smallest edit that tests the hypothesis.
4. **Decision:** measure before and after in one session, then keep the change or revert it.
   **A reverted experiment is still recorded**: "we tried X, it didn't help, here's the data" is a
   result, and an interviewer will value it as much as a win.

### 0.2 Why not just make the obvious improvements?
Because "obvious" was wrong twice already:
- **The hybrid search, phase 2:** checking the levels nearest the best one by one before a
  binary search "had to be faster". It was **slower** on every benchmark, and was dropped.
- **The phase 4 tail:** deep inserts (shifting thousands of levels) looked like the cause of the
  slow tail. The flow analysis showed they happen in only 0.02% of level creations; the tail
  points elsewhere.

Modern CPUs are hard to predict: caches, branch prediction and out-of-order execution all hide or
amplify costs. Only measurement settles it.

### 0.3 What this laptop can measure, and what needs bare metal
WSL2 runs Linux inside a Windows virtual machine, which **hides the CPU's hardware counters**
(checked: `perf stat -e cycles` reports "not supported"). Some measurements work anyway:

| Measurement | Tool | WSL2 | Bare metal |
|---|---|---|---|
| Throughput (messages per second) | `nanobook_replay --throughput`, `nanobook_pipeline --throughput` | ✓ | ✓ |
| Latency percentiles (p50, p99) | the phase 3 `rdtsc` harness | ✓ (p99.9 and above are noisy) | ✓ |
| Page faults, context switches | `perf stat -e page-faults`, `getrusage` (software counters) | ✓ (checked) | ✓ |
| Where the time goes, by function | `perf record -e cpu-clock` (timer sampling) | ✓ (checked) | ✓, more precise with `cycles` |
| Cache misses, TLB misses, branch misses | `perf stat` hardware events | ✗ | ✓ |
| Which cache lines bounce between cores | `perf c2c` | ✗ | ✓ |

So phase 6 runs every experiment it can on WSL2: **before/after on the same machine, in the
same session** is valid even in a virtual machine (the perf log's standing rule). Explanations
that need hardware counters stay labelled **hypotheses** until the bare-metal session (§5).

### 0.4 Noise, and how to beat it
The same binary run twice gives slightly different numbers: the laptop warms up and turbo
changes, and Windows wakes up in the background. Phases 3–5 measured this: about 2–5% at p50 and
throughput, 7–17% at p99, much more above. So:
- **Before and after run in one session, alternating** (A B A B), so a drift in the machine hits
  both equally. Running all of A and then all of B would blame the drift on the change.
- **The decision metric is chosen before running**, so we can't pick whichever number happened to
  improve.
- **A gain smaller than the noise is no gain.**

---

## 1. Goal and exit criteria

**Phase 6 is done when** (README roadmap):
1. Each change kept or tried has a **before/after table** in `docs/perf-log.md`.
2. Every kept change passes the **correctness gates** (§4): same output, same tests, nothing
   allocates.
3. A phase 6 report sums up what helped, what didn't, and why.

No target speed-up is promised: the target is **knowing why**. A list of honest negative results
meets the criterion as well as a list of wins.

---

## 2. The leads

Each lead starts from a number already in the perf log. They're in the proposed order (§8 Q1):
cheap and well-evidenced first, the big redesign last.

### E1. Three threads slower than one (phase 5)
- **Symptom:** the whole day runs at 5.8 M messages/s on one thread, but 4.2 M on three
  (174 against 237 ns per message). Ring 1 is found full 4.5 times per message.
- **Hypothesis: cache-line ping-pong on `tail_`.** The feed outruns the engine, so ring 1 is
  always full. The feed keeps re-reading the engine's `tail_`, and every pop must take the line
  back (~90 ns, one hop). The gap is 64 ns per message, about that size.
- **Change: two small variants, both measured** (§8 Q3):
  - **(a) The consumer publishes `tail_` in batches:** every N records (say 64), and always
    before it waits on an empty ring, so the producer can never wait for a slot that is already
    free. The line then changes only once per N records, however often the feed reads it. This
    is the "batch update" idea of MCRingBuffer (Lee et al., 2010).
  - **(b) The producer backs off:** after finding the ring full, it waits a little longer before
    each re-read, so it reads the line less often.
- **Measure:** `nanobook_pipeline --throughput` (three threads, ns per message, ring-1-full
  count) is the decision metric; `nanobook_ringbench` and paced latency are guards. The
  latency must not get worse below capacity, where the ring is never full.
- **If right:** three threads at least match one thread. Their limit is the engine's own work,
  without parsing or logging.

### E2. First-touch page faults in the sorted-level arrays (phase 4)
- **Symptom:** p99.99 of about 11 µs on the real day, against 2.3 µs for one book in cache
  (phase 3).
- **Hypothesis:** `SortedLevels` only `reserve()`s its two arrays (161 MB for the day), so
  each 4 KiB page is first written mid-replay. The OS then stops the program to provide the page
  (a "minor page fault", a few µs each). That's tens of thousands of faults, about 1 in 10,000
  messages: the p99.99 level. The order pool and id table are zero-filled at construction, so
  they don't fault.
- **Change:** write to every page of the level arrays at construction, as the latency recorder
  already does. Writing past `size()` of a `std::vector` is undefined behaviour, so the arrays
  need their own storage; the exact form is decided in the step.
- **Measure:**
  - **Decision metric:** page faults during the timed pass, counted with `getrusage` around it.
    Expected: tens of thousands → near 0.
  - **The effect:** p99.99 per message on the real day.
  - **Guards:** throughput and start-up time.

### E3. The hybrid search, on real flow (phase 2, re-opened by phase 4)
- **Symptom:** on real flow, 48% of new orders land at or improve the best price, and 35% of new
  levels are created at the best end of the array. That clusters far more tightly than the
  phase 2 benchmarks that rejected the hybrid.
- **Hypothesis:** checking the K levels nearest the best one by one, before a binary search,
  now wins.
- **Change:** a template parameter K on `SortedLevels`. K = 0 is today's binary search, so the
  default can stay unchanged.
- **Measure:** the real day, `nanobook_replay --throughput --latency`, for K = 0, 4, 8 and 16.
  The decision metric is throughput; p50 and p99 per message kind are guards. Synthetic phase 3
  scenarios check that nothing else regresses.

### E4. Compiler options: LTO, PGO, `-march=native`
- **Symptom:** none specific: the README lists these as phase 6 tools. They change no code,
  only how it's compiled.
- **What each does:**
  - **LTO (link-time optimisation)** lets the compiler optimise across `.cpp` files. NanoBook's
    engine is header-only, so each tool already sees all of it in one file: **expect little or
    nothing**, and that is worth confirming.
  - **PGO (profile-guided optimisation)**: run once with counters, then recompile using where
    the program really spent its time and which way each branch really went. Commonly reported
    gains are 5–15% on branchy code. **Trap:** training and measuring on the same data flatters
    it, so it trains on one slice of the day (9:30–12:00) and is measured on the whole day.
  - **`-march=native`** lets the compiler use every instruction this CPU has (for example
    `tzcnt`, used by the tick array's bitmaps). The binary may then not run on an older CPU,
    and the flag must be stated next to any published number.
- **Measure:** each option against the default build, on the real day (throughput and p50/p99),
  and on the phase 3 scenarios.

### E5. Ring padding: 64 against 128 bytes (phase 5 decision Q4)
- **Symptom:** none measured: 128 bytes was chosen on the README's advice (Intel CPUs fetch
  the neighbouring cache line too) and never tested.
- **Measure:** `nanobook_ringbench` (throughput and round trip) and the pipeline throughput,
  with `kNoFalseSharing` = 64 against 128. `perf c2c` on bare metal would show the line traffic
  directly (§5).
- **Expect:** a small difference either way. This is a cheap check of a rule we followed blindly.

### E6. Huge pages for the books' memory
- **Symptom:** the same book is 2.5–5× slower per message on the real day than in cache
  (phase 4), because the day jumps between 8,713 books.
- **Hypothesis:** besides cache misses, **TLB misses**. The CPU translates every address
  through a small table (the TLB) of recently used pages. With 4 KiB pages, the books' memory
  (hundreds of MB) needs over 100,000 pages, far more than the TLB holds, so many messages wait
  for a page-table walk. With 2 MiB **huge pages**, a few hundred pages cover it all.
- **Change:** allocate the books' big arrays from one start-up region, aligned to 2 MiB and
  marked with `madvise(MADV_HUGEPAGE)`.
  - WSL2 allows it: `transparent_hugepage` is set to `madvise`, checked.
  - Rule 1 still holds: all of it happens at start-up.
  - If the OS refuses huge pages, the tool reports it and runs on normal pages.
- **Measure:** the real day, throughput and p50/p99. The TLB-miss counts themselves need bare
  metal; on WSL2 the throughput change is the evidence.

### E7. The moving window (planned since phase 2, fast-book design §6.3)
- **Symptom:** the tick array is 1.2–4× faster than the sorted list in phase 2's benchmarks, but
  can't hold a real day: stub quotes stretch a stock's range from $0.0001 to $200,000.
- **Idea:** a tick array covering only a window of prices around the best (say 4,096 ticks),
  with the rare far-away prices in a sorted list. Almost all activity is near the best (93% of
  new orders within $1), so almost all of it gets tick-array speed.
- **Why last:**
  - It's a new data structure with new failure modes (a level at the window's edge, a slide
    during a sweep, rare large pauses when the window moves).
  - So it gets **its own design section, approved separately**, its own fuzzer runs (including
    inputs that force slides), and the full-day differential replay.
- **Measure:** the real day against the sorted list: throughput, p50, p99 and the slide pauses.

### E8. Deep inserts (fast-book design §6.1)
- **Symptom:** 0.02% of level creations shift 1,024 or more levels; about 40 GB of shifting a
  day.
- **Bound:** at roughly 40 GB/s of copying, that's about 1 s of the day's ~38 s of book work,
  **at most about 3%**.
- **Plan:** only after E7, which changes where deep levels live, and only if the flow analysis
  still shows it.

### Not leads (no symptom)
`[[likely]]`, `__builtin_expect`, manual prefetching and forced inlining: rule 10 allows them
only with a measured gain. None is proposed until a profile (`perf record`) points at a specific
branch or load.

---

## 3. Method

### 3.1 The A/B session (step 1)
`scripts/run_ab_session.sh BEFORE_REF AFTER_REF OUT_DIR -- COMMAND ...`:
- **Two builds:** it builds two commits (or a commit and the working tree) into two separate
  folders, through `git worktree`, so both binaries exist before any timing starts.
- **Alternating runs:** it runs COMMAND with each binary in turn, A B A B, two rounds at least,
  each run its own process, pinned.
- **Recorded output:** every output gets a progress log with timestamps and the environment
  block, as phase 5's session script did.

It refuses a dirty tree for the `before` side, like the other session scripts, so every number
names a real commit. This one script replaces the one-off worktree recipe used for the
48-against-64-byte order slot experiment in phase 2.

### 3.2 Counting page faults where they matter
The timing tools (`nanobook_replay`, `nanobook_pipeline`) gain one line in their reports: the
**minor and major page faults during the timed part only**, from `getrusage` before and after.
This is cheap, works on WSL2, and E2 and E6 need it.

### 3.3 The decision rule (§8 Q2)
A change is **kept** if:
- its decision metric is better **in every round**;
- the gain is larger than the round-to-round difference of either build;
- no guard metric got worse beyond its noise;
- every correctness gate passes (§4).

Otherwise it is **reverted**, and the perf-log entry says so with the numbers. A marginal result
(a small gain for a lot of complexity) goes to the user to decide.

### 3.4 The perf-log entry for each experiment
Each entry follows the existing format, with five parts:
1. The symptom and the hypothesis.
2. The change, as a diff or in one sentence.
3. The method: commits, cores, rounds, and the decision metric.
4. The **before/after table**, with both rounds.
5. The decision, and **what the result says about the hypothesis**.

---

## 4. Correctness gates (every kept change)

| Gate | Applies to |
|---|---|
| All 6 presets, `clang-format-19`, `clang-tidy-19`, the 2-CPU threaded tests (as CI) | Every change |
| A planted bug in the new code must fail a test | Every change with new logic |
| `nanobook_fuzz` differential against the reference book (10⁷+ operations) | Any change to the books |
| Full-day differential replay: 0 mismatches in 363 million checks (phase 4 step 5) | Any change to the books |
| Full-day pipeline log checksum unchanged: `748abedf40533b82` (phase 5 step 3) | Any change to the ring or the stages |
| TSan clean, including the 2-record-ring test | Any change to the ring or the stages |
| Zero allocations after start-up | Every change |
| Machine code of untouched functions unchanged (as phases 2 and 4 checked) | Changes meant to touch one function |

---

## 5. Bare metal only

Run in the bare-metal session together with phases 3, 4 and 5's published numbers, and **not
blocking** phase 6:

| # | What | Why it needs bare metal |
|---|---|---|
| B1 | The real day's cache misses and TLB misses, per message | Separates phase 4's "2.5–5× slower" into cache, TLB and shifting; confirms or rejects E6's hypothesis |
| B2 | `perf c2c` on the pipeline | Shows the `tail_` line bouncing directly (E1), and the 64/128 padding effect (E5) |
| B3 | FastBook `match`: 9.3 ns in throughput mode against 13.1 ns in Google Benchmark (phase 3) | Needs `perf record` with cycle-accurate samples and `perf stat` |
| B4 | Tails on isolated cores (`isolcpus`, `nohz_full`) | Separates the machine's stalls from the code's: the pipeline's flat 65–72 µs p99, the 11 µs p99.99 |
| B5 | **Re-check every kept change**: re-run its A/B with `scripts/run_ab_session.sh` | A virtual machine translates every address twice (Linux's page tables, then the hypervisor's), so page faults (E2) and TLB misses (E6) cost more there and their gains can look bigger than on real hardware. E2 and E6 **must** be re-checked; the rest are a quick confirmation. Until then their perf-log entries say "kept on WSL2 evidence, to re-check on bare metal" |

---

## 6. Files (expected)

| File | Change |
|---|---|
| `scripts/run_ab_session.sh` | New: the A/B session (§3.1) |
| `tools/nanobook_replay.cpp`, `tools/nanobook_pipeline.cpp` | Page faults during the timed part (§3.2) |
| `include/nanobook/pipeline/spsc_ring.hpp` | E1, E5 |
| `include/nanobook/fast/sorted_levels.hpp` | E2, E3 |
| `CMakeLists.txt`, `CMakePresets.json` | E4: LTO and PGO options, separate presets |
| A huge-page region in `support/` or `include/nanobook/fast/` | E6 (decided in the step) |
| New level structure, `include/nanobook/fast/` | E7, with its own design section |
| `docs/perf-log.md` | One before/after entry per experiment |

---

## 7. Plan, one commit per step

Every timed step needs an idle laptop, and I ask before each session.

| Step | What | Data file? |
|---|---|---|
| 0 ✓ | This design note, decisions recorded. Approved 2026-10-21 | No |
| 1 | `scripts/run_ab_session.sh`; page faults in the timing tools' reports | No |
| 2 | E1: ring batching, variants (a) and (b), A/B on the pipeline | Yes |
| 3 | E2: touch the level arrays at construction, A/B on the real day | Yes |
| 4 | E3: hybrid search, K = 0/4/8/16, on the real day | Yes |
| 5 | E4: LTO, PGO (trained on 9:30–12:00), `-march=native`, against the default | Yes |
| 6 | E5: ring padding 64 against 128 | Yes |
| 7 | E6: huge pages for the books' memory | Yes |
| 8a, 8b, 8c | E7: moving window: design section; code, tests and fuzzing; measurement | Yes |
| 9 | E8: deep inserts, only if the flow analysis still shows them after step 8 | Yes |
| 10 | Phase 6 report | Yes |

Each experiment is kept or reverted on its own numbers. Later experiments are measured on top of
the kept ones, so each table's "before" is the code as it stood at that point.

---

## 8. Decisions (approved 2026-10-21)

| # | Question | Proposal | Why |
|---|---|---|---|
| Q1 | Order of the experiments | As §7: E1, E2, E3, E4, E5, E6, then E7 and E8 | Cheap and well-evidenced first; the risky redesign last, on top of what's already been learned |
| Q2 | When is a change kept? | Better in every round, by more than the noise, with no guard worse and every gate passed (§3.3); otherwise reverted and still recorded | Stops noise or one lucky run from deciding; negative results are results |
| Q3 | E1: which ring fix? | **Measure both** (a) batched `tail_` publishing and (b) producer back-off; keep the winner | Each is about 10 lines, and comparing them shows which side of the ping-pong matters |
| Q4 | Compiler options | The default presets stay portable (`-O3`, no `-march=native`); LTO, PGO and native get their own presets; LTO joins the default only if it wins | A portable default anyone can build; every published number states its flags |
| Q5 | Decide on WSL2 numbers? | **Yes** for throughput, p50 and p99 (stable to 2–10%); claims about p99.9 and above, and every "because of cache/TLB" explanation, wait for bare metal (§5). Every kept change is re-checked there (B5), E2 and E6 without fail | Phase 6 shouldn't stall on the USB stick; the noisy and VM-sensitive parts are labelled and re-checked |
| Q6 | The moving window in phase 6? | **Yes, last**, with its own design section approved separately before code | The biggest possible gain and the biggest risk; planned since phase 2 |
| Q7 | Huge pages: how | One 2 MiB-aligned region allocated at start-up, `madvise(MADV_HUGEPAGE)`, normal pages if refused (and reported) | Rule 1 holds; works on WSL2 today; no root needed (unlike reserved `hugetlbfs` pages) |

---

## 9. Out of scope for phase 6

| Idea | Why not |
|---|---|
| Several engine threads, one per group of stocks (sharding) | A new architecture, not an optimisation; one book stays single-threaded (rule 5). Optional, later |
| Kernel bypass, network input | Not in NanoBook (README §1) |
| SIMD searches over levels | No symptom points at the search cost yet; E3 and a profile come first |
| Rewriting in assembly | Rule 24: every line must stay explainable, and the compiler already emits what we check |

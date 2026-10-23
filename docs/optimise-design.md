# Phase 6 design: optimise, one measured change at a time

> **Status:** approved (2026-10-21): every decision in §8 as proposed, plus a re-check of every
> kept change on bare metal (§5, B5). Changes from here on are made by an edit plus a note in the
> commit message. Step 1: the A/B script as built ("after" is the working tree, §3.1); page
> faults in the tools (§3.2). Step 2: E1 measured, (b) kept; E1c and E1d added (§2). Step 2b:
> E1c measured (§2). Step 3: E2 measured, kept provisionally with its bare-metal criteria (§5).
> Step 4: E3 measured, K = 4 kept (§2). Step 5: E4 measured, none kept as the default; PGO
> trains on the day up to 12:00, not 9:30–12:00, so the books build as on the day (§2). Step 6:
> E5 measured, 128 stays (§2). Step 7: E6 measured, the huge-page arena kept; E2's
> PrefaultAllocator folded into it (§2, §5 B5-E6). A profile before E7 put half the replay in
> the ID table: E9 added and done first (the user's decision, §2, §7). Step 8: E9a reverted;
> E9b reverted pending bare metal (§2, §5 B6).

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
- **Result (step 2, 2026-10-21; perf log "E1, the ring's ping-pong"):**
  - The hypothesis holds: (a), (b) and (a)+(b) all cut three threads from about 205 to about
    175 ns per message, 13–16%, and tie within noise.
  - **(b) is kept**, the simplest (decision rule §3.3; the user's choice). Paced latency at 2 M/s
    is unchanged.
  - Three threads remain about 1.09× one thread on this test. Two follow-ups:
    - **E1c:** paced latency on one thread against three, the comparison that decides whether
      the pipeline pays at all;
    - **E1d:** the same ping-pong on ring 2, the publisher polling the engine's `head_`.
- **E1c result (step 2b, perf log "E1c, one thread against three"):**
  - At normal load (1–2 M/s) one thread is faster per record: p50 about 205 ns against
    about 500, because the pipeline adds two hand-overs (about 135 ns).
  - Near capacity three threads hold up better: they sustain 4.9 M records/s or more against one
    thread's 4.3.
  - Real logging to a file is a tie.
  - Both modes stay. E1d can trim a little but can't remove the hops, so it waits until the
    rest of the plan is done.

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
- **Result (step 3, 2026-10-21; perf log "E2, page faults in the level arrays"):**
  - Step 1's counter showed only 4,793 faults a day, one per about 77,000 messages: about 8×
    too rare to set p99.99 alone.
  - Touching the pages at start-up (`PrefaultAllocator`) took the faults to 0 and improved
    p99.99 about 12% in both rounds, but p99 got about 2% worse, maybe code-layout noise.
  - **Kept provisionally** (the user's decision), to be re-checked on bare metal and reverted if
    worse (§5, B5).

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
- **Result (step 4, 2026-10-21; perf log "E3, the hybrid search on real flow"):**
  - **K = 4 kept**: 10% more throughput in both rounds, with every latency guard equal or better.
  - K = 8 and 16 gave no more throughput and slowed adds.
  - Phase 2's verdict is reversed by real data (fast-book design §13).

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
- **Result (step 5, 2026-10-22; perf log "E4, compiler options"):** none kept as the default.
  - LTO gained nothing (the engine is header-only, as predicted).
  - `-march=native` was 1.8% slower, mostly in parsing (`movbe`, AVX2).
  - PGO gained 1.4% throughput but made replaces 25% slower at p50 and p99 3–5% worse.
  - The options, presets and scripts stay as tools for the bare-metal re-check.

### E5. Ring padding: 64 against 128 bytes (phase 5 decision Q4)
- **Symptom:** none measured: 128 bytes was chosen on the README's advice (Intel CPUs fetch
  the neighbouring cache line too) and never tested.
- **Measure:** `nanobook_ringbench` (throughput and round trip) and the pipeline throughput,
  with `kNoFalseSharing` = 64 against 128. `perf c2c` on bare metal would show the line traffic
  directly (§5).
- **Expect:** a small difference either way. This is a cheap check of a rule we followed blindly.
- **Result (step 6, 2026-10-22; perf log "E5, ring padding"):** 64 bytes is worse.
  - On the ring alone: round trip worse in all 4 pairs, throughput in 3 of 4.
  - On the pipeline: no difference.
  - **128 stays.** The ring benchmark turned out bimodal on WSL2: virtual CPUs land on P-cores,
    E-cores or hyperthread twins. So between-core numbers wait for bare metal (B2).

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
- **Result (step 7, 2026-10-22; perf log "E6, huge pages for the books"):** kept.
  - First tested with no code: glibc's switch gave 4.8% more throughput and p99 7.6% better.
  - Then built as Q7 says, with standard C++:
    - `support/huge_page_arena.hpp`: a 2 MiB-aligned, `madvise`d, pre-touched region under
      `std::pmr::monotonic_buffer_resource`;
    - the books' three arrays became `std::pmr::vector` and are built in it by the timed tools.
  - **5.6% more throughput, p99 11% better.** E2's `PrefaultAllocator` is replaced by the arena's
    start-up touch.
  - Re-check on bare metal, criteria in §5 (B5-E6).

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

### E9. The ID table's memory waits (added 2026-10-22, from the profile)
- **Symptom:** a profile of the real day's replay (perf log "Where the real day's time goes")
  puts **half** the time in `IdTable::find`, and another 12% on the first read of the order's
  slot. Levels, E7's target, are about 23%.
- **Hypothesis:** both are memory waits. The ID tables total over 100 MB (30 MB of L3), and a
  delete, execute or replace reads its id's entry and then its slot, each a likely miss.
- **Changes, measured one at a time:**
  - **(a) Prefetch one message ahead.** The parser hands the handler the next message's bytes
    before the current one is applied. For D, E, C, X, U and adds, the book builder works out
    the id's ID-table position and prefetches it, so the load overlaps the current message's
    work (about 90 ns, about one memory trip). Rule 10 allows a manual prefetch only with a
    measured gain, which is what the A/B gives.
  - **(b) Compact entries:** 16 → 8 bytes, so twice as many fit in each cache level.
- **Measure:** the real day, `nanobook_replay --throughput --latency`. Throughput decides;
  p50, p99 and p99.99 are guards.
- **Safety:** a prefetch is only a hint and can't change a result. The full-day differential
  replay and the golden tests must stay identical.
- **Result (b), 2026-10-22 (perf log "E9b"):** undecided on WSL2, reverted (the user's choice).
  - The lookups got faster: executions 18–30% and deletes 6–24% at p50, in all 4 pairs.
  - Throughput was better in 2 pairs and worse in 2, in two unusually noisy sessions.
  - Re-tested on bare metal (§5, B6).
- **Result (a), 2026-10-22 (perf log "E9a"):** not kept.
  - Throughput about 6% worse in both rounds; only executions gained.
  - The hook's work on every message seems to cost more than the overlap saves. That's a
    hypothesis for bare metal.

### Not leads (no symptom)
`[[likely]]`, `__builtin_expect`, manual prefetching and forced inlining: rule 10 allows them
only with a measured gain. None is proposed until a profile (`perf record`) points at a specific
branch or load.

---

## 3. Method

### 3.1 The A/B session (step 1)
`scripts/run_ab_session.sh BEFORE_REF OUT_DIR [--rounds N] -- TOOL ARGS...`:
- **Two builds:**
  - "before" is BEFORE_REF, built in a temporary `git worktree`;
  - "after" is the current working tree, uncommitted changes included, saved as
    `OUT_DIR/after.diff`.
  Both binaries exist before any timing starts.
- **Alternating runs:** two rounds at least, in the order A B, B A, A B... (ABBA), so a machine
  warming up or cooling down hits both sides equally. Each run is its own process; the tools pin
  their own threads.
- **Recorded output:** each run's output goes to its own file, with a timestamped progress log.
  `{out}` in the arguments becomes the run's own path, so `--csv {out}.csv` never overwrites
  another run's file.
- **A/A runs:** with BEFORE_REF = HEAD and no changes, both sides are the same code, which
  measures the noise alone.

**As built:** "after" is the working tree, not a second commit as first planned. An experiment
is measured before it is committed (the user commits), and `after.diff` records exactly what was
measured. To compare two commits, check out the later one. This one script replaces the one-off
worktree recipe used for the 48-against-64-byte order slot experiment in phase 2.

### 3.2 Counting page faults where they matter
The timing tools report the **minor and major page faults** of each timed part, from
`getrusage` (`support/page_faults.hpp`), read just outside the clock. This is cheap, works on
WSL2, and E2 and E6 need it.
- **`nanobook_replay`:** the timed part only, window by window (`replay_in_windows`), so the
  file's own loading is excluded.
- **`nanobook_pipeline`:** whole runs, because its threads time themselves. A three-thread run
  includes building the two rings (zero-filled, 7 MB, about 1,800 pages) and the thread stacks;
  a paced run also includes the warm-up. The report says so.

**First look (the first 200 MB of the day, not a measurement):** parsing alone takes 0 faults
while timed, and the books only about 20. Those 200 MB are before the market opens, while the
books are small; the full day decides E2.

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
| B6 | **E9b's re-test (criteria fixed 2026-10-22).** Apply `docs/data/phase6_wsl/e9b_compact/after.diff` to the commit after this one, then `scripts/run_ab_session.sh HEAD OUT --rounds 4 -- nanobook_replay FILE --throughput --latency --core C --csv {out}.csv`. **Keep** (re-add it) if throughput is better in every round by more than the noise, and p50 and p99 are not worse beyond theirs. Otherwise leave it out, and record either way | On WSL2 the lookups got faster, but throughput sat inside the noise |
| B5-E6 | **E6's re-check, criteria fixed before the data (2026-10-22).** Check out the E6 commit, then `scripts/run_ab_session.sh <E6's parent> OUT -- nanobook_replay FILE --throughput --latency --core C --csv {out}.csv`, and confirm the report shows the arena on 2 MiB pages. **Keep** if throughput is better in every round by more than the noise and p50 and p99 are not worse beyond theirs. **Revert** (the `pmr` vectors can stay; the timed tools stop using the arena) if throughput isn't better beyond the noise, or p50 or p99 is worse beyond it in every round. The outcome goes in the perf log either way | In a VM a TLB miss walks two sets of page tables, so WSL2 may overstate the gain |
| B5-E2 | **E2's re-check, criteria fixed before the data (2026-10-21).** Check out the E2 commit, then `scripts/run_ab_session.sh <E2's parent> OUT -- nanobook_replay FILE --throughput --latency --core C --csv {out}.csv`. **Keep** if p99.99 is better in every round, by more than the noise, and p50, p99 and throughput are not worse beyond their noise. **Revert** if p99.99 isn't better beyond the noise, or if p50, p99 or throughput is worse beyond the noise in every round, unless p99.99 improves by clearly more (then weighed with the bare-metal numbers). The outcome goes in the perf log either way. **Since E6** the arena does E2's touching at start-up, so reverting E2 means the arena stops touching its pages: the huge pages are then mapped on first use (about 200 faults a day instead of 4,758) | E2 cost 2% at p99 on WSL2, maybe code-layout noise; bare metal's lower noise decides |

---

## 6. Files (expected)

| File | Change |
|---|---|
| `scripts/run_ab_session.sh` | New: the A/B session (§3.1) |
| `support/page_faults.hpp`, `tests/test_page_faults.cpp` | Step 1: the page-fault counter and its tests (§3.2) |
| `support/itch_timing.hpp` | Step 1: faults counted while timed, window by window (§3.2) |
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
| 1 ✓ | `scripts/run_ab_session.sh`; page faults in the timing tools' reports. Built 2026-10-21 | No |
| 2 ✓ | E1: ring batching, variants (a) and (b), and (a)+(b) at the user's request, A/B on the pipeline. (b) kept. Done 2026-10-21 | Yes |
| 2b ✓ | E1c: `--sequential` in the latency mode; paced latency on one thread against three (and a slice logged to a real file). Done 2026-10-21 | Yes |
| 3 ✓ | E2: touch the level arrays at construction, A/B on the real day. Kept provisionally (bare-metal re-check, §5 B5). Done 2026-10-21 | Yes |
| 4 ✓ | E3: hybrid search, K = 0/4/8/16, on the real day. K = 4 kept. Done 2026-10-21 | Yes |
| 5 ✓ | E4: LTO, PGO (trained on the day up to 12:00), `-march=native`, against the default. None kept as the default; kept as presets. Done 2026-10-22 | Yes |
| 6 ✓ | E5: ring padding 64 against 128. 128 stays. Done 2026-10-22 | Yes |
| 7 ✓ | E6: huge pages for the books' memory. Kept (bare-metal re-check, §5 B5-E6). Done 2026-10-22 | Yes |
| 8 ✓ | **E9 (added from the profile, before E7):** (a) prefetch the ID-table entry one message ahead: reverted, 6% slower; (b) 8-byte entries: undecided on WSL2, reverted, re-test on bare metal (§5, B6). Done 2026-10-22 | Yes |
| 8a, 8b, 8c | E7: moving window: design section; code, tests and fuzzing; measurement. Its ceiling is the levels' share (about 23%, profile), re-measured after E9 | Yes |
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

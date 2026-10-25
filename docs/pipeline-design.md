# Phase 5 design: the pipeline

> **Status:** approved (2026-10-20): every decision in §12 as proposed. Changes from here on are
> made by an edit plus a note in the commit message. Items marked **[MEASURE]** are settled by a
> measurement in the step named. Step 1 (2026-10-20): the assembly check and the planted
> relaxed-store bug (§3.3, §8). Step 2: the record layouts as built (§4); spin-then-yield waiting
> after CI timed out on 4 vCPUs (§3.5). Step 3: the log as built (§6). Step 4a: the timing modes as
> built, with the warm-up settle time (§9.2a); step 4 split into 4a and 4b (§11). Step 4b: how
> full the rings get (§3.4). Step 5: the phase 5 report, `docs/phase5-report.md`.
> **Phase 5 complete (2026-10-24):** the published numbers from bare metal
> (`docs/bare-metal-report.md`); re-run at the release commit on 2026-10-25
> (`docs/data/bare_metal/release/phase5/`).

Phases 1–4 ran everything on **one thread**: read a message, apply it to the book, then the next.
Phase 5 splits that work into **three stages on three threads**, connected by **lock-free
single-producer single-consumer (SPSC) ring buffers** that NanoBook writes itself (plan §5,
roadmap phase 5). The book stays single-threaded (rule 5): threads are for the stages around it,
never for the book itself.

---

## 0. Background in plain words

### 0.1 Why split the work into stages?
Think of an **assembly line**. One worker who reads the order slip, updates the ledger *and*
files the receipt does all three in turn. Three workers in a row (reader, bookkeeper, clerk) each
do one job, and while the bookkeeper updates the ledger for slip 2, the reader is already reading
slip 3. Each worker keeps only their own tools within reach, so nobody fetches anything from across
the room.

In NanoBook:

| Stage | Thread | Job |
|---|---|---|
| **Feed** | A | Read the ITCH file and parse each message into a small fixed-size record |
| **Engine** | B | Apply each record to its stock's book (phase 4's book builder), and say what changed: the stock's new best bid and ask |
| **Publisher / logger** | C | Write each update to a binary log file, and measure how long the whole trip took |

Only thread B touches the books, so they need no locks at all. A real trading system is built the
same way: the network handler, the book and the strategy or publisher on separate cores.

### 0.2 Why not a mutex?
Threads have to hand records to each other. The textbook way is a queue guarded by a **mutex**
(a lock). But locking and unlocking costs tens of nanoseconds even with no contention, and when
the lock *is* taken, the waiting thread may be put to sleep by the operating system: microseconds,
and a system call on the hot path (rule 6). With exactly **one writer and one reader** per queue,
no lock is needed at all: each side only ever writes its own counter.

### 0.3 The ring buffer
A **ring buffer** is a fixed array used in a circle. The writer (producer) puts records at the
`head`, the reader (consumer) takes them from the `tail`, and both wrap around to the start when
they reach the end. Picture a sushi conveyor belt with a fixed number of plates: the chef puts
plates on, the diner takes them off, and if every plate is full the chef waits.

- **Empty:** `head == tail`, nothing to read.
- **Full:** `head - tail == capacity`, nowhere to write.
- The producer only ever writes `head`; the consumer only ever writes `tail`. Each only *reads*
  the other's.

### 0.4 Why memory ordering matters
A modern CPU and compiler may **reorder** memory operations to go faster, as long as one thread
alone can't tell. With two threads, it can. If the producer does

1. write the record into slot 7
2. set `head = 8` ("slot 7 is ready")

and the consumer sees step 2 *before* step 1 reached it, it reads a half-written record. C++
atomics with **memory orders** say exactly which reorderings are forbidden:

- **release** on the producer's `head` store: every write *before* it (the record) becomes visible
  to any thread that sees the new `head`;
- **acquire** on the consumer's `head` load: nothing *after* it (reading the record) can happen
  before it.

That pair is the whole trick. It is cheaper than the default `seq_cst` (no full fence on x86), and
§3.3 shows why it is exactly enough, and why `relaxed` would be a bug. `volatile` does none of this
(plan §4).

---

## 1. Goal and exit criteria

**Goal:** `nanobook_pipeline FILE` replays an ITCH day through three pinned threads and reports
throughput and end-to-end latency, with the same results as the single-threaded replay.

**Phase 5 is done when** (README roadmap):
1. **TSan is clean**: every multi-threaded test passes under ThreadSanitizer, in CI (§8).
2. **End-to-end latency percentiles are reported**: from a message being ready to send to its
   update reaching the publisher (§9). WSL2 numbers are marked not for publishing; the published
   ones come from bare metal (rule 20), like phases 3 and 4.

Also required (rules 1, 11): no allocation after start-up, and **byte-identical output** to a
single-threaded run of the same day.

---

## 2. Architecture

```
 ITCH file --> [A] feed ------ring 1------> [B] engine ------ring 2------> [C] publisher
 (mmap)        parse into                   BookBuilder<FastBook>           binary log file,
               FeedRecord                   -> TopOfBook update             latency samples
               (pinned core)                (pinned core)                   (pinned core)
```

- **Ring 1** carries `FeedRecord`s: one per ITCH message the books care about (A, F, E, C, X, D,
  U, plus R, H, S for names and states), decoded into a fixed-size struct. The parser runs on A;
  B never sees raw bytes.
- **Ring 2** carries `TopOfBook` updates: after each order message, the stock's best bid price and
  size and best ask price and size (what a strategy reads first), plus the record's timestamps.
- **End of stream:** A pushes a final `FeedRecord` of kind `End`; B passes on an `End` update and
  stops; C sees it and stops. No flags, no shared state outside the rings.

### 2.1 Why decode on A (decision Q1)
Splitting the work only helps if each stage has some. Parsing is 3% of the single-threaded time
(phase 4), the book 97%, so the split won't double throughput: B is the bottleneck either way.
What it buys is **overlap and isolation**: B's core does nothing but book work, with the parser's
memory traffic (the 11 GB file) on another core. Sending raw bytes instead would put the parsing
back on B.

---

## 3. The SPSC ring buffer

### 3.1 Interface
```
template <typename T, std::size_t Capacity>   // T trivially copyable, Capacity a power of two
class SpscRing {
    bool try_push(const T &item);   // producer only; false if full
    bool try_pop(T &out);           // consumer only; false if empty
    void push(const T &item);       // spins until there is room (§3.5)
    T pop();                        // spins until there is a record
};
```
`include/nanobook/pipeline/spsc_ring.hpp`, header-only, no allocation after construction (the
array is a member or allocated once).

### 3.2 Layout
```
 [ head_ (producer writes) | pad ][ cached_tail_ (producer's copy) | pad ]
 [ tail_ (consumer writes) | pad ][ cached_head_ (consumer's copy) | pad ]
 [ slots_[Capacity] ]
```
- `head_` and `tail_` are `std::atomic<std::uint64_t>` counters that only ever increase (2^64
  records never wrap in practice); a slot is `counter & (Capacity - 1)`, one AND instead of a
  division, which is why Capacity is a power of two.
- **Each atomic sits on its own cache line**, padded to **128 bytes** (rule 9). If `head_` and
  `tail_` shared a 64-byte line, every push would invalidate the consumer's copy of that line and
  every pop the producer's: **false sharing**, the two cores fighting over one line although they
  never touch the same variable. 128 rather than 64 because Intel CPUs prefetch the *adjacent* line
  too (rule 9; decision Q4).
- **Cached indices:** the producer keeps its own copy of the last `tail_` it read, and only reloads
  the real one when the ring *looks* full; the consumer likewise for `head_`. Most pushes then
  touch only the producer's own lines. (This is the design of Erik Rigtorp's `SPSCQueue` and of
  most production rings.)

### 3.3 The memory orders, line by line

**Producer, `try_push(item)`:**
1. `h = head_.load(relaxed)`: only the producer writes `head_`, so it always sees its own last
   value; no ordering needed.
2. If `h - cached_tail_ == Capacity` (looks full): `cached_tail_ = tail_.load(acquire)`; if still
   full, return false. **acquire** pairs with the consumer's **release** store of `tail_` (step 4
   below): once the producer sees the consumer has moved past a slot, the consumer's *read* of that
   slot is guaranteed finished, so overwriting it is safe.
3. `slots_[h & mask] = item`: a plain write.
4. `head_.store(h + 1, release)`: **release** makes step 3's write visible to any thread that later
   sees `h + 1` with an acquire load. This is the "slot 7 is ready" signal.

**Consumer, `try_pop(out)`:**
1. `t = tail_.load(relaxed)`: only the consumer writes `tail_`.
2. If `t == cached_head_` (looks empty): `cached_head_ = head_.load(acquire)`; if still equal,
   return false. **acquire** pairs with the producer's step 4: seeing `head_ > t` guarantees the
   slot's contents are visible.
3. `out = slots_[t & mask]`: a plain read.
4. `tail_.store(t + 1, release)`: **release** makes "I have finished reading slot t" visible
   before the producer can reuse it.

**Why not `relaxed` everywhere?** Then the producer's `head_` store could become visible before
the record itself: the consumer reads stale or half-written data. On x86 the *hardware* keeps
stores in order, so this might even work in tests, but the **compiler** may still reorder, and on
ARM the hardware would too. It is a data race, which is undefined behaviour; TSan reports it. One
of the planted bugs (§8) does exactly this, to prove TSan catches it.

**Why not `seq_cst` (the default)?** It adds a full fence (`mfence` or a locked instruction on
x86) to every store, to keep one global order of all atomic operations across all threads. An SPSC
ring only needs each *pair* of threads to agree, which acquire/release gives for free on x86.
**Checked in step 1** (`-O2`, GCC 14 and Clang 19): in `try_push` and `try_pop` every acquire load
and release store is a plain `mov`, with no fence or locked instruction anywhere; a `seq_cst`
store of the same atomic compiles to `xchg` (a locked instruction, roughly 20 cycles). The member
offsets in the same assembly (`head_` at 0, `cached_tail_` at 128, `tail_` at 256, `cached_head_`
at 384) confirm the 128-byte padding.

### 3.4 Capacity
Ring 1 and ring 2: **65,536 records** each (decision Q8). `FeedRecord` and `TopOfBook` are at most
64 bytes (one cache line, §4), so a ring is about 4 MB: big enough to absorb bursts, small enough
for the L3 cache (30 MB). **Measured (step 4b):** unpaced, ring 1 is always full (found full
4.5 times per message: the engine is the bottleneck) and ring 2 never; paced below capacity, both
are nearly empty. The capacity only matters for absorbing stalls (65,536 records is 33 ms at
2 M/s); ring 2 filled once in all the paced runs, during a publisher stall.

### 3.5 When a ring is full or empty
- **Full: the producer waits** (spins) until there is room (backpressure, decision Q3). Dropping
  market data would make the books wrong, which no copy of Nasdaq can allow. Every wait is
  counted, so a slow stage shows up in the report.
- **Empty: the consumer spins** with the `pause` instruction (`_mm_pause`), which tells the CPU
  "this is a wait loop": it saves power and avoids a costly pipeline flush when the value finally
  changes. No sleeping, no system call (rule 6): each stage owns a core.
- **Changed in step 2 (CI found it): spin, then yield.** A pure spin assumes every thread has its
  own core. GitHub's runners have 4 shared vCPUs; when the thread being waited for isn't running,
  a spinner burns its whole time slice on every hand-over, and the 2-record-ring test hit CI's
  60 s limit (0.04 s on 24 CPUs; over 100 s when squeezed onto 2 with `taskset`). Now the stages
  spin with `pause` for 1,024 failed tries (tens of microseconds), then call
  `std::this_thread::yield()` before each further try, so the waited-for thread gets the core
  (2 CPUs: 3.3 s; 1 CPU: 5 s; under ASan on 2 CPUs: 7 s). `PipelineOptions::spin_only` keeps the
  pure spin for timed runs on pinned, dedicated cores, where a yield could only add latency. The
  ring's own `push`/`pop` stay pure spin (the ring tests and `nanobook_ringbench`).

---

## 4. The records

| Record | Ring | Fields | Size |
|---|---|---|---|
| `FeedRecord` | 1 | kind (A, F, E, C, X, D, U, R, H, S, End), locate, side, ITCH timestamp, order ref, new ref, shares, price, symbol/state, `sent` (TSC ticks: when it was ready to send) | at most 64 bytes, `static_assert`ed |
| `TopOfBook` | 2 | locate, sequence number, ITCH timestamp, best bid price and qty, best ask price and qty, `sent` (copied from the record) | 64 bytes (58 of fields), `static_assert`ed |

Both are trivially copyable, so a push is one cache line's copy. **As built (step 2):**
`FeedRecord` is **48 bytes** (four 8-byte fields, two 4-byte, then locate, kind and side): an R's
8-character symbol travels in `ref`, an F's attribution in `new_ref`, an S's event code and an H's
trading state in `side`. Only what the book builder reads crosses: an add's stock name and an
execution's match number and price stay behind. `TopOfBook` is **64 bytes**. The ITCH messages
the books never use (P, Q, I, ...) are counted by the feed and not sent. `sent` travels with the record so C can
compute the end-to-end latency (§9).

---

## 5. The stages

- **A, feed:** walks the file in phase 4's pre-loaded windows (`replay_in_windows()`), with a
  parser `Handler` that turns each message into a `FeedRecord` and pushes it. Other message types
  are counted and dropped.
- **B, engine:** pops records and feeds them to phase 4's `BookBuilder<FastBook>` (no changes to
  the book code). After each order message, it reads the stock's best bid and ask and pushes a
  `TopOfBook`. Sized by the counting pass before the threads start (phase 4 §7).
- **C, publisher:** pops updates, appends each to the binary log (§6), and records the latency of
  every Nth one (§9).
- **Start-up:** sizing, book construction, ring allocation and thread creation all happen before
  the first message: rule 1 holds from the first record on. Each thread pins itself to its core
  (`pthread_setaffinity_np`) and checks it.

---

## 6. The binary log (rule 7)

C writes every `TopOfBook` as its raw 48 bytes into a 1 MiB buffer, and writes the buffer to the
file with one `write()` system call when it is full (thread C is allowed system calls: it is off
the hot path). A small tool, `nanobook_logdump FILE`, turns the binary log into text afterwards.
Formatting text at 10 million records a second would make C the bottleneck; binary now and text
later is how production loggers work (decision Q6).

The log is **deterministic** (rule 11): it holds only fields that come from the input (no TSC
times), so the same day must give a **byte-identical** log however the threads were scheduled.

**As built (step 3):** `support/binary_log.hpp`. A 16-byte header (`NBLOG001`, the record size)
then one 56-byte `LogRecord` per update: sequence, ITCH time, locate, best bid and ask with their
sizes, and two reserved fields always 0, so the struct has **no padding** (padding bytes hold
whatever was in memory and would make identical updates give different bytes;
`static_assert(std::has_unique_object_representations_v<LogRecord>)` proves there is none).
`write()` is retried until every byte is out (a write may take part of the buffer, or be
interrupted). An optional FNV-1a checksum of every byte lets two runs be compared without keeping
their files: a full day's log is about 20 GB, and WSL2's disk grows but never shrinks on its own.
`nanobook_pipeline FILE [--log OUT] [--checksum] [--sequential] [--cores A,B,C]` runs a day;
`nanobook_logdump LOG [--head N]` prints it as CSV, streaming record by record.

**Result (step 3, 2026-10-21):**

| Input | One thread | Three threads (cores 2, 4, 6) |
|---|---|---|
| Golden day, busy day (tests) | log file | **byte-identical** file |
| 200 MB real slice (6,590,504 updates) | 369 MB log, checksum `bede88f09b98acb5` | **identical** (`cmp`), same checksum |
| Full day (363,118,215 updates, logs to `/dev/null`) | 20,334,620,056 bytes, checksum `748abedf40533b82` | **same bytes, same checksum** |

The full-day runs took 86.0 s (one thread) and 81.7 s (three threads): barely faster, as expected
when the engine's book work is 97% of everything (phase 4); not benchmarks (checksums on, laptop
not idle). Ring 1 was constantly full: the feed outruns the engine. Step 4 measures it properly.

---

## 7. Proving it right

| Test | What it shows |
|---|---|
| Ring, one thread | Push and pop in order, full and empty, wrap-around past the end of the array, capacity |
| Ring, two threads (stress) | Producer pushes 0, 1, 2, ... 10 million; the consumer checks every value arrives exactly once, in order. Under TSan in CI |
| Pipeline = single thread | The golden day (phase 4) through three threads gives the **same log, byte for byte**, as the same day through one thread; and on the full real day, the same final books |
| Shutdown | `End` reaches C on every run; no thread is left spinning |
| Zero allocations | None after start-up, all three threads (the counting `operator new` from phase 2) |
| Planted bugs | `relaxed` instead of `release` on `head_` (TSan must report it); a missing wrap mask (the stress test must fail) |

---

## 8. ThreadSanitizer in CI

TSan instruments every memory access and reports two threads touching the same memory without a
happens-before order between them. The `gcc-tsan` and `clang-tsan` presets exist since phase 0,
but CI doesn't run them: **step 1 adds both to CI** (decision Q7), and every test runs under them
(the single-threaded tests too, cheaply). TSan is about 5–15x slower, so the stress test sizes
itself down under TSan.

**Running TSan locally (WSL2):** this kernel randomises memory addresses more than TSan's runtime
expects, and it stops at start-up with `unexpected memory mapping`. CI sets `vm.mmap_rnd_bits=28`
(needs root); locally, `setarch $(uname -m) -R` runs a command with address randomisation off, no
root needed, and its children inherit that: `setarch $(uname -m) -R cmake --build --preset
gcc-tsan` (the build runs the tests once to list them) and `setarch $(uname -m) -R ctest --preset
gcc-tsan`.

**Checked in step 1:** with the producer's `head_` store planted as `relaxed`, TSan reports a data
race between `try_push`'s slot write and `try_pop`'s slot read, **while the test's own checks
still pass**: on x86 the hardware happened to keep the stores in order. The bug is real (undefined
behaviour, and wrong on ARM) but invisible to ordinary testing; only TSan sees it.

---

## 9. What gets measured

### 9.1 Throughput (unpaced)
A pushes as fast as it can; the rings fill; the slowest stage sets the speed. Report: messages
per second end to end, and how often each producer found its ring full (which stage waits on which).

### 9.2 End-to-end latency (paced): beware coordinated omission (rule 18)
Measuring latency while A pushes as fast as possible is meaningless: every queue is always full,
so the latency is just queue length. And if A stamps each record *when it actually sends it*, a
stall in A hides itself: a record that should have gone out at t = 0 but went at t = 50 µs is
stamped 50 µs, and its 50 µs wait never appears. That is **coordinated omission**.

So the latency runs are **paced**: A sends record i at its **intended** time `t0 + i / R` for a
fixed rate R (messages per second), busy-waiting until then, and stamps `sent` with the
**intended** time, not the actual one. If A falls behind, the delay is counted. C records
`now - sent` for every Nth update. Runs at several rates (say 1, 2, 4, 8 million messages per
second) give the classic latency-against-load curve, ending where the pipeline saturates
(decision Q5).

All three cores read the same time-stamp counter: on this CPU it is invariant (`constant_tsc`,
`nonstop_tsc`) and synchronised across cores, so a stamp taken on A can be compared on C.

### 9.2a As built (step 4)
`nanobook_pipeline FILE --cores A,B,C --throughput` and `--latency --rate R [--from HH:MM:SS]
[--until HH:MM:SS] [--csv FILE]`:
- **Pinned, spinning only** (`spin_only`): each stage owns a core, so no yields (§3.5).
- **The whole file is loaded into memory first**, untimed: in the pipeline the feed thread
  reads the file, and a window load mid-run would starve the engine and look like a slow
  engine. 11 GB fits next to the books in WSL2's 15 GB (phase 4's runs peaked at 11.3 GB).
- **Throughput:** the whole day on one thread (`run_sequential`), then on three, logs to
  `/dev/null`: messages per second each, and the ratio.
- **Latency over a slice of the day:** the feed replays unpaced up to `--from` (the warm-up that
  builds realistic books), then paces R records per second until `--until`, and drops the rest
  (the whole day at 1 M/s would take 6 minutes per rate). `support/pacing.hpp`'s `Pacer` stamps
  each record with its intended time in whole-number arithmetic (no drift), after a **100 ms
  settle time**: without it, the first paced records queued behind the warm-up's backlog (up to
  65,536 records, 4–8 ms) and the run measured the backlog, not the pipeline (found in a trial
  run). The stamp now receives the record, so the pacer can see the warm-up end.
- **Each sample spans two cores** (stamped on the feed, read on the publisher), so the phase 3
  recorder gained `record_across_cores()`: its `record()` drops samples whose two stamps come
  from different cores, correctly for single-core timing.

**Result (step 4b, WSL2, not for publishing; details in `docs/perf-log.md`, 2026-10-21):**
- p50 is about 500 ns end to end up to 2 M records/s. The minimum, about 175 ns, is two
  core-to-core hops.
- The pipeline saturates at about 4.4 M records/s.
- p99 is 65–72 µs at every rate up to 2 M/s: the signature of the machine stopping, not of
  queueing.
- Three threads are slower than one unpaced (4.2 against 5.8 M messages/s). Leading
  hypothesis: the engine's `tail_` cache line bouncing while ring 1 is full. A phase 6 candidate.

### 9.3 Environment
Phase 3's environment block and NOT FOR PUBLISHING banner, plus the three cores used. On WSL2 the
threads go on **different physical cores** (CPUs 2, 4, 6: CPUs 2 and 3 are the two hyperthreads of
one core), so two stages never share one core's caches and execution units.

---

## 10. Files

| File | What |
|---|---|
| `include/nanobook/pipeline/spsc_ring.hpp` | The ring buffer (§3) |
| `include/nanobook/pipeline/records.hpp` | `FeedRecord`, `TopOfBook` (§4) |
| `include/nanobook/pipeline/stages.hpp` | The three stages and the start/stop logic (§5) |
| `support/binary_log.hpp` | The publisher's buffered binary writer (§6) |
| `tools/nanobook_pipeline.cpp` | The CLI: throughput and paced latency runs (§9) |
| `tools/nanobook_logdump.cpp` | Binary log to text (§6) |
| `tests/pipeline/...` | Ring (one and two threads), pipeline = single thread, shutdown, allocations |
| `.github/workflows/ci.yml` | The two TSan jobs (§8) |

---

## 11. Plan, one commit per step

| Step | What | Needs the data file? |
|---|---|---|
| 0 | This design note, decisions recorded | No |
| 1 ✓ | `SpscRing` + one- and two-thread tests + TSan in CI; the assembly check (§3.3); `nanobook_ringbench` (throughput and round trip of one ring; run in step 4's session). Built 2026-10-20 | No |
| 2 ✓ | Records and the three stages (`records.hpp`, `stages.hpp`); pipeline = single thread on the golden day and on a busy day with 2-record rings; shutdown; zero allocations. Built 2026-10-20 | No |
| 3 ✓ | Binary log + `nanobook_logdump`; byte-identical log against a single-threaded run, on the full day (checksums) and on a 200 MB real slice (files, `cmp`). Built 2026-10-21 | Yes |
| 4a ✓ | Timing modes in `nanobook_pipeline` (§9.2a): throughput (one thread vs three), paced latency with a settle time; `Pacer`, `record_across_cores()`. Built 2026-10-21 | No |
| 4b ✓ | The runs on an idle laptop: `nanobook_ringbench`, throughput, latency at several rates (WSL2, not for publishing). Run 2026-10-21: results in `docs/perf-log.md`, data in `docs/data/phase5_wsl/` | Yes |
| 5 ✓ | Phase 5 report (`docs/phase5-report.md`, `.pdf`; charts from `scripts/plot_phase5.py`); published numbers later, on bare metal, with phases 3 and 4. Done 2026-10-21 | Yes |

---

## 12. Decisions (approved 2026-10-20)

| # | Question | Decision | Why |
|---|---|---|---|
| Q1 | What crosses ring 1? (§2.1) | **Decoded fixed-size `FeedRecord`s** (A parses) | Gives A real work and keeps B's core for book work only |
| Q2 | What does B publish? (§2) | **Top of book** (best bid and ask, price and size) after each order message | What a strategy reads first; small and fixed-size |
| Q3 | A full ring? (§3.5) | **The producer waits**, and every wait is counted | Dropping market data breaks the books |
| Q4 | Padding between the atomics (§3.2) | **128 bytes** | Rule 9: Intel's adjacent-line prefetch; 64 vs 128 measured in phase 6 with `perf c2c` (bare metal) |
| Q5 | Latency method (§9.2) | **Paced** at fixed rates with intended send times, plus an unpaced throughput run | Avoids coordinated omission (rule 18) |
| Q6 | Logger output (§6) | **Binary file** written by C, `nanobook_logdump` formats it afterwards | Text formatting at this rate would make C the bottleneck |
| Q7 | TSan in CI (§8) | **Both** `gcc-tsan` and `clang-tsan` | Rule 14; the two TSans differ slightly in what they catch |
| Q8 | Ring capacity (§3.4) | **65,536 records** each (about 4 MB) | Absorbs bursts, fits in L3; how full they get is measured in step 4 |

---

## 13. Out of scope for phase 5

| Idea | Where it belongs |
|---|---|
| Several engine threads, one per group of stocks ("sharding") | Later, optional: one book stays single-threaded either way |
| Multi-producer or multi-consumer queues | Not needed: every link here has one writer and one reader |
| Optimising the ring (batching, 64 vs 128 padding, huge pages) | Phase 6, with before/after numbers (rule 10) |
| Network input (UDP, kernel bypass) | Not in NanoBook (plan §1) |
| A live view of the published book | Optional, after phase 7 (plan §2), as a separate process |

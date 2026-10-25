# NanoBook: interview notes

Short answers to "why did you do it that way?", each with the number that settled it and where it
was measured. Every timing here is from **bare-metal Linux on an isolated core** (Intel
i7-14650HX, GCC 14.3, `-O3`) unless it says WSL2. Longer explanations are in the phase reports;
`docs/final-report.md` tells the whole story in order.

---

## 1. The 30-second version

NanoBook is a limit order book in C++23. It has a simple **reference book** (`std::map`,
`std::list`) as the answer key, and a **fast book** with no heap allocation while trading. It
rebuilds every stock's book from a real Nasdaq day, 368 million ITCH messages, at **about 68 ns
per message** on one core, with the slowest 1 in 10,000 messages at about **0.61 µs**. A
three-thread pipeline over my own lock-free ring buffers adds about **0.65 µs at p99** from feed to
publisher, up to 4 million messages a second. The fast book matches the reference book after
every one of 363 million real order messages and 162 million random operations.

---

## 2. Decisions

Each entry: **chose**, **instead of**, **because** (with the evidence), **would revisit if**.

### 2.1 C++23
- **Chose:** C++23 on GCC 14 and Clang 19, Linux.
- **Instead of:** C++20 or C++26.
- **Because:** C++23 has `std::expected` (errors without exceptions) and `std::byteswap` (ITCH is
  big-endian); C++20 has neither. C++26 adds nothing the core needs, and its compiler support is
  uneven. The standard doesn't make code faster: layout, allocation and cache behaviour do.
- **Would revisit if:** I wanted reflection for tooling (pretty-printing messages), kept out of
  the core.

### 2.2 Price levels: a sorted array, best price at the end
- **Chose:** `SortedLevels`: each side's levels in one array, best price last, binary search to
  find a price (after checking the 4 levels nearest the best, E3).
- **Instead of:** `std::map` (one heap node per level), or a tick-indexed array (one slot per
  possible price, O(1) lookup).
- **Because:** against `std::map`, sweeps were 1.7–1.9× faster and allocations went to 0 (phase 2).
  The tick array was up to 3.9× faster on cancels, but its memory follows the price band, not the
  number of levels. On the real day, sizing tick arrays for every stock's price range would need
  **489,000 GB**: stale quotes far from the market stretch the range (phase 4). The sorted array
  works for any price.
- **Would revisit if:** a venue had narrow, fixed bands; or I could make the moving window's
  slowest messages behave (§2.9).

### 2.3 Order id → order: an open-addressing hash table
- **Chose:** `IdTable`: linear probing, Fibonacci hashing, backward-shift delete, sized at start-up.
  Later, entries cut from 16 to 8 bytes (a 32-bit slot plus a 32-bit tag from the id's hash; the
  full id is confirmed from the order itself) (E9b).
- **Instead of:** `std::unordered_map` (one node per order), or a table indexed directly by id.
- **Because:** replacing `unordered_map` made adds 2.1× and cancels 1.4× faster (phase 2). A direct
  table needs one entry per id of the whole day in every book, and ITCH ids are exchange-wide.
  The profile then showed **half the replay inside the ID table** (phase 6), so halving the entry
  fits twice as many per cache line: **7% faster on the day, p50 24% better**, in 4 of 4 rounds.
  The cost: confirming a tag match reads the order's id from the pool, a second read that waits
  on the first. In the synthetic `match` test, where everything is in L1, that made `match`
  8–17% slower; on the real day matches got 13–21% faster at p50 (perf log, 2026-10-25).
- **Would revisit if:** ids were dense per book (then a direct table wins outright).

### 2.4 Orders in a pool with intrusive lists, `uint32_t` indices
- **Chose:** every order slot made at start-up in one array; each price level's queue is a doubly
  linked list through the slots, using 4-byte slot numbers as links.
- **Instead of:** `std::list` nodes (one `new` and one `delete` per order), and 8-byte pointers.
- **Because:** allocations per added order went from **2.01 to 0**, and adds became 3× faster
  (phase 2). Indices are half the size of pointers and stay valid if the array moves. A full
  pool returns `BookFull`, which the contract already defines.
- **Would revisit if:** never, for this workload. The order slot stayed 48 bytes: padding it to a
  whole 64-byte cache line made no measurable difference (phase 2, step 7).

### 2.5 No exceptions in the engine
- **Chose:** the library is built with `-fno-exceptions`; errors are status codes or
  `std::expected`.
- **Instead of:** throwing on bad input.
- **Because:** a thrown exception costs microseconds, and bad input (an unknown order id) is
  normal on a feed, not exceptional. `-fno-exceptions` makes a `throw` a compile error, so the
  rule can't be broken by accident.
- **Would revisit if:** never in the hot path. Tests and tools keep exceptions (GoogleTest needs
  them).

### 2.6 The SPSC ring: acquire/release, not `seq_cst`
- **Chose:** my own single-producer, single-consumer ring: the producer publishes `head_` with a
  **release** store, the consumer reads it with an **acquire** load (and the reverse for `tail_`).
  The two counters sit 128 bytes apart.
- **Instead of:** a mutex, or `seq_cst` atomics everywhere, or `relaxed` everywhere.
- **Because:** release/acquire is exactly the guarantee needed: "the record is written before the
  counter says so". On x86 both compile to a plain `mov` (checked in the assembly); `seq_cst`
  stores compile to `xchg`, a locked instruction of about 20 cycles. `relaxed` would be a data
  race: ThreadSanitizer caught it when I planted it, and no ordinary test did (phase 5). 128-byte
  spacing beat 64 in 4 of 4 round-trip runs (E5); the likely reason is Intel's adjacent-line
  prefetcher, which pulls lines in pairs.
- **Would revisit if:** porting to ARM (the code stays correct, but the cost changes: acquire and
  release are real barriers there).

### 2.7 Three threads are slower than one, and that's fine
- **Chose:** a feed → engine → publisher pipeline, each on its own pinned core.
- **Instead of:** doing everything on one thread.
- **Because:** the book is 96% of the work, so the engine thread is the bottleneck either way.
  Unpaced over the whole day, three threads run at **0.78×** the speed of one (bare metal): every
  hand-over moves cache lines between cores. With the feed backing off when ring 1 is full (E1b),
  that improved by 16%. The pipeline still wins when there's slow work to take off the engine:
  writing the 20 GB log to disk, three threads were faster (81.7 s against 86.0 s, WSL2).
- **Would revisit if:** there were several books to shard across engines, or heavy parsing (the
  parse is only 3% here).

### 2.8 Huge pages for the books' memory
- **Chose:** the books' three big arrays live in one arena on 2 MiB pages, touched once at start-up.
- **Instead of:** normal 4 KiB pages, mapped on first use.
- **Because:** 7% faster on the day, p50 and p99 better (E6, bare metal). Bigger pages mean fewer
  address translations to cache. Touching every page at start-up means **no page faults while
  trading**. Its own rule said remove the touching loop: without it, the timed run had 1 fault
  and no measurable cost (B5-E2b). I kept it anyway, because that 1 fault was luck (the
  untouched arrays happened to share pages with touched ones), and the loop makes 0 a guarantee.
- **Would revisit if:** a machine without huge pages (the kernel then leaves normal pages, and the
  tool reports how much memory got huge pages).

### 2.9 The moving window: built, proven correct, not the default
- **Chose:** keep `FastBook` (sorted levels) as the default; the window book stays a tested option
  (`--book window`).
- **Instead of:** a tick-array window around the best price, sorted array beyond it.
- **Because:** it was **15% faster on average**, but the slowest 1 in 10,000 messages got
  **2.5–6× slower** (bare metal, 4 rounds). A trading system is judged on its tail.
- **Would revisit if:** I found why its tail is slow. It makes 9% more trips to main memory per
  message, which fits "a sparse window spreads levels over more cache lines", but doesn't prove it.

### 2.10 Measuring: my own `rdtsc` harness, paced load, bare metal only
- **Chose:** time each operation with the CPU's time-stamp counter (calibrated, timer overhead
  subtracted), into a histogram; pace the pipeline to a rate and timestamp each record at its
  **intended** send time; publish only from bare metal with isolated cores.
- **Instead of:** Google Benchmark averages; timestamping when a record was actually sent; WSL2.
- **Because:** averages hide tails. Timestamping at actual send hides stalls (coordinated
  omission: a record delayed 50 µs would be stamped late and look fast). And WSL2 lied: its
  pipeline p99 was **65–72 µs at every rate**; on isolated cores it's **0.65 µs**, 100× lower.
  That tail was the virtual machine, not the code.
- **Would revisit if:** never. This is the part I'd defend hardest.

---

## 3. What didn't work

| Idea | Why it should have helped | What happened |
|---|---|---|
| Hybrid search (phase 2) | Check levels near the best before binary search | Slower on synthetic books, dropped. On real flow (phase 6) it won by 10%: real orders cluster near the best |
| 64-byte order slot | Never straddle two cache lines | No difference beyond ±6%: 4.8 MB of orders fits in L3 anyway |
| `-march=native` | Newer instructions | 1.8% slower: the parser got slower with `movbe` and AVX2 |
| LTO | Inlining across files | No gain: the hot code is all in headers, already inlined |
| PGO | Lay out code for the real branch pattern | 1.4% more throughput, but replace p50 25% worse |
| Prefetch the next message's ID entry | Hide the memory wait | 6% slower. Likely (untested): the hook's work on all 368 M messages cost more than the wait it hid |
| Moving window | O(1) price lookup near the best | 15% faster on average, slowest messages 2.5–6× slower |
| Batch ring 2's counter updates | Fewer cache-line hand-overs | 7–12% faster unpaced, but paced p90 worse: updates waited for a batch |
| 64-byte ring padding | Less memory | Round trip worse in 4 of 4 runs (likely the prefetcher pulling lines in pairs) |

The pattern: three of these improved the average and hurt the tail. Only rules written down
before the timing stopped them from being kept on their average.

---

## 4. Likely questions

**How do you know the fast book is correct?** It shares no code with the reference book, and they
agree on 162 million random operations (the differential fuzzer) and after every one of 363 million
real order messages. 790 unit tests, ASan, UBSan and TSan in CI, a libFuzzer target on the parser,
and dozens of planted bugs (every one caught) to check the tests have teeth.

**Why is your p99.99 ten times your p50?** At 1 in 10,000, a few rare events decide it: an
insert deep in a book that shifts thousands of levels, a cache-cold stock, an interrupt that still
reaches an isolated core. It varies between rounds (match: 42 vs 174 ns), so I report both.

**Why not `std::unordered_map` with `reserve()`?** It still allocates a node per insert, and
following the node pointer is a second cache miss. Open addressing keeps the entry in the array.

**What does a cancel cost, and where does the time go?** About 56 ns at p50 on the real day: find
the slot in the ID table (usually one cache miss), read the order (often another), unlink it,
update the level. The hardware counters say about 0.5 trips to main memory per message:
**cache misses, not TLB misses**, dominate (B1).

**Why is the real day 2.5–5× slower than your microbenchmarks?** The microbenchmark's book fits in
cache. The real day has 8,713 books in a 328 MB arena, against a 30 MB L3: most messages touch
memory no cache holds.

**Why acquire/release and not sequential consistency?** It's the exact guarantee needed, free on
x86 (plain `mov`), and `seq_cst` adds a locked `xchg` per store.

**What would you change for production?** Kernel-bypass networking instead of a file, sharding
books across engine threads, a recovery path (snapshots plus the binary log), and measuring on the
production CPU, because several results here (padding, `-march=native`) are machine-specific.

**What was hardest?** Not fooling myself. The VM hid good news (E9b's real 7% gain) and invented
bad news (a 65 µs tail). Fixing the decision rule before running the experiment mattered more than
any single optimisation.

**What is coordinated omission?** Measuring latency from when a message was actually sent, so a
stall that delays sending is never counted. The fix: stamp each message with when it was *due*.

---

## 5. What I'd do next
1. Find the moving window's tail (cache-miss sampling per message kind), then reconsider it.
2. Shard the 8,713 books across several engine threads by stock.
3. Replace the file feed with a UDP multicast feed (MoldUDP64), the way ITCH is really delivered.

# Phase 2 design: the fast book

> **Status:** approved (2026-10-17). Items marked **[MEASURE]** are decided by a benchmark, not in
> advance (plan rule 10). Changes from here on are made by an edit plus a note in the commit message.
> Decided: price levels (§6): sorted list is the main design, the tick array an experiment, the
> moving window postponed to phase 6. Reusing the exchange's id table (§5.3) stays an experiment.
> **Phase 2 complete (2026-10-18):** all §11 steps done; both exit criteria met (fuzzer 162 million
> operations with no difference, zero allocations while trading). See `docs/phase2-report.md`.

The fast book is a **second, separate implementation** of the same contract as the reference book
(`docs/design.md`). It must produce **exactly the same events, in the same order**, for every
input. It is not an edited copy of the reference book: it uses different data structures, and the
reference book stays unchanged as the trusted answer key.

---

## 1. Goal and exit criteria

**Goal:** remove the costs the phase 1 baseline measured (`docs/perf-log.md`, report §8):

| Phase 1 cost | Cause | Phase 2 answer |
|---|---|---|
| 2.01 heap allocations per added order | A `std::list` node per order, a `std::map` node per order and per level | An order pool and fixed tables, allocated once at start-up (§3, §5, §6) |
| Cancel is the slowest operation (137 ns) | Two tree lookups that jump around memory | A flat hash table for id → order (§5), and orders that remember their price (§4) |
| Sweeps cost ~40 ns per level | A tree erase and a node free per level | Levels in flat arrays, with no per-level allocation (§6) |

**Phase 2 is done when** (README roadmap):
1. The **differential fuzzer** (§8.3) passes **10 million or more** random operations against the
   reference book.
2. The **zero-allocation test** (§8.4) shows **exactly 0** heap allocations while trading.

Speed targets are not exit criteria. The README's *target* for the fast book is tens to low hundreds
of ns at p50, and phase 3's `rdtsc` harness measures that. Phase 2 records before/after averages
for every change (rule 22).

---

## 2. What stays the same

| Unchanged | Where |
|---|---|
| Every behaviour: inputs, events, check order, matching rules, self-match prevention, modify | `docs/design.md` §3–§7 |
| Types, messages, `EventBuffer`, the `OrderBook` concept | `include/nanobook/core/` |
| `Exchange<Book>`: it already accepts any `OrderBook` | `include/nanobook/exchange.hpp` |
| The 96 typed tests: the fast book joins `BookTypes` | `tests/test_helpers.hpp` |
| `ReferenceBook`: never changed in phase 2 | `include/nanobook/reference/` |

If writing the fast book reveals a gap in the contract, the fix goes into `design.md` first, then
into **both** books.

---

## 3. Order pool

**Problem:** the reference book asks the system for memory every time an order arrives. That is
slow, and how slow varies unpredictably.

**Design:** reserve room for all `max_orders` orders **once, in the constructor**, like a car park
with numbered spaces.
- Storage: one `std::vector<Order>` of exactly `max_orders` slots. It is sized once and never resized.
- Orders are referred to by **slot number** (`uint32_t`), never by pointer. Slot numbers are half the
  size of a pointer, so more fit in each 64-byte cache line, and they stay valid if the storage
  ever moved.
- `kNone = UINT32_MAX` means "no slot".
- **Free list:** the empty slots are chained together through their own `next` field, so no extra
  memory is needed. Taking a slot = pop the head of the free list. Freeing a slot = push it back.
  Both are a few instructions.
- **Last freed, first reused:** a slot that was just freed is probably still in the CPU cache, so
  it is the fastest one to reuse.
- **Full pool = `BookFull`.** The pool holds exactly `max_orders` slots, so "no free slot" is the
  same condition the contract already rejects (`design.md` §4). There is no new behaviour.

Slot numbers never appear in events, so which slot an order gets has no effect on the output.
Determinism (rule 11) is unaffected.

---

## 4. Orders and price-level queues

### 4.1 The order slot

| Field | Size | Why it's there |
|---|---|---|
| `OrderId id` | 8 | Trade and Canceled events name it |
| `Qty leaves` | 8 | Remaining qty: read on every match step |
| `Qty filled` | 8 | Modify's total-quantity rule (`design.md` §7). Total = `leaves + filled`. |
| `Price price` | 8 | So cancel and modify can find the order's level without searching for it |
| `uint32_t prev`, `next` | 4 + 4 | Neighbours in the level's queue (§4.2). `next` doubles as the free-list link. |
| `OwnerId owner` | 4 | Self-match prevention: checked on every match step |
| `Side side` | 1 | Which side's levels to remove it from |
| padding | 3 | |
| **Total** | **48 bytes** | `static_assert`ed (rule 9) |

There's no sequence number: **the queue order is the time priority**. Modify that loses priority
moves the order to the back of the queue. That is the same behaviour, and invariant 5 becomes
"the queue's links are consistent".

**48 vs 64 bytes: measured (2026-10-18), 48 kept.** At 48 bytes, half the orders straddle two
cache lines; padding to 64 (`alignas(64)`) makes each order exactly one line, but uses a third more
memory. Measured on the same machine in one session, both fast-book variants, all 10 benchmarks
(`docs/perf-log.md`): every difference was between −6% and +6%. 12 of the 20 were inside the
round-to-round noise; the 8 beyond it are small and point both ways (on `FastBook` mostly towards
64 bytes being slower, e.g. 4–5% on the 10- and 100-level sweeps; on `TickFastBook` mixed, e.g.
6% faster on the 10-level sweep). 64 bytes never clearly won, so
the order stays at 48 bytes: simpler, and a quarter less memory. Worth re-checking only if phase 3's
tail-latency numbers point at order access.

### 4.2 Intrusive FIFO queue per level

**Problem:** `std::list` puts each order in a separate box, allocated on arrival and freed on exit.

**Design:** "intrusive" means the links live **inside the order slot** (`prev`, `next`), so there is
no separate box. A level only stores the ends of its queue:

| Level field | Size | Meaning |
|---|---|---|
| `Price price` | 8 | |
| `Qty total` | 8 | Sum of `leaves` in the queue, kept up to date. Invariant 3 becomes a real check. |
| `uint32_t head`, `tail` | 4 + 4 | Oldest and newest order slot |
| `uint32_t count` | 4 | Number of orders, for checks and queries |
| padding | 4 | |
| **Total** | **32 bytes** | `static_assert`ed |

Operations, all a few instructions with no memory request:
- **Push back** (a new order, or a modify that loses priority): link after `tail`.
- **Unlink anywhere** (cancel, self-match cancel): connect `prev` and `next` to each other.
- **Pop front** (an order fully filled while matching): move `head` to `next`.

---

## 5. Order id lookup

**Problem:** a cancel says "order #7". The book needs **which slot holds order #7**. Ids are large and
spread out (exchange-wide, with gaps), while slots are small and packed (0 … `max_orders`−1), so a
translation table is needed. The reference book uses a tree: about 14 steps, each somewhere else in
memory.

### 5.1 Chosen: a flat hash table, built in

- **One array, allocated once**, with a power-of-two number of entries **≥ 2 × `max_orders`**. It is
  never more than half full, so searches stay short.
- **Entry:** `{ OrderId id; uint32_t slot; }`. That's 16 bytes with padding. `id == 0` means
  "empty". The exchange never issues id 0, and a book asserts that its ids increase from 0, so 0 can
  never be a real order.
- **Where an id goes:** a fixed scramble formula turns the id into an array position: multiply by
  the constant 11400714819323198485 (2⁶⁴ divided by the golden ratio) and keep the top bits.
  Exchange ids arrive in near-sequential runs, and a straight `id % size` would pile neighbours
  into neighbouring entries. The multiply spreads them out, and it costs one multiplication.
- **Collisions:** if that entry is taken, try the next one (and wrap at the end). With the table at
  most half full, a search usually takes 1–2 steps.
- **Removing without leaving holes:** simply emptying an entry would make a later search stop early
  at the gap and miss an order that had been pushed further along. Instead, after removing, entries
  further along that belong earlier are **shifted back** into the hole ("backward-shift deletion").
  The table never fills up with "deleted" markers, even after a whole day of cancels.
- **Memory:** 2 × `max_orders` (rounded up to a power of two) × 16 bytes. For 1M orders, that's 32 MB.

The table gets its **own unit tests**, especially for removal: removing from the middle of a
collision run, and wrapping around the end of the array.

**[MEASURE]** A 12-byte packed entry, or storing only the slot and reading the id from the pool to
compare. Both are smaller, but cost an extra read. Benchmark against the 16-byte entry.

### 5.2 Rejected: one entry per possible id

"Entry number = order id" would need one step with no formula. But ids are exchange-wide, so every
book would need an entry for every id of the whole day: 8,000 symbols × hundreds of millions of
entries. It's not possible.

### 5.3 Experiment, not the plan: reuse the exchange's id table

The exchange already has one entry per id. Widening it to also store the slot number gives a
one-step lookup. Against it:
- Memory grows from 2 to about 8 bytes per order of the day (100M orders: 800 MB).
- It reverses the exchange design: cancels avoid reading that table (`design.md` §3.1, cold path),
  because it is huge and old entries are rarely in cache.
- A book would no longer work on its own, which changes the contract and complicates book-level
  tests and the fuzzer.

Only worth trying after §5.1 is measured, and only with a before/after table.

---

## 6. Price levels

**Problem:** the reference book keeps levels in a tree, one allocation per level, scattered in
memory.

**Decision (2026-10-17):** the contract stays as it is: the fast book works for **any** price band
(`design.md` §12). So:
- the **sorted list** (§6.1) is the main design: `FastBook` uses it;
- a **plain tick array** (§6.2) is built as a **benchmark experiment**, to measure how much faster a
  tick array could be;
- a **moving window** (§6.3) is postponed to phase 6, and only built if those numbers justify it.

The fast book is a template over its level structure, so both variants run the same tests, fuzzer
and benchmarks with identical code:

```
BasicFastBook<SortedLevels>    // main design
BasicFastBook<TickLevels>      // experiment: narrow bands only
using FastBook = BasicFastBook<SortedLevels>;
```

### 6.1 Main design: a sorted list of only the prices that have orders

One `std::vector<Level>` per side, with capacity reserved for `max_orders` levels at start-up (there
can never be more levels than resting orders), so it never grows while trading.
- **Memory grows with the number of levels in use, not with the band**, so any band works: a $5
  stock and BRK.A cost the same.
- **Best price at the END of the array.** Most activity is near the best price, and inserting or
  removing near the end only shifts a few entries, like adding a book at the end of a shelf
  instead of the front.
- **Finding a level** (a new order, a cancel, a modify): **binary search** (halve the range each
  step, like a dictionary: about 7 looks for 100 levels).
- Orders store their **price, not a level position**, because positions shift whenever a level is
  inserted or removed.

**Measured and dropped (2026-10-17): the hybrid search.** The plan was to check the `K` levels
nearest the best one by one before switching to binary search, betting that lookups cluster near the
best price. Measured on the same machine in one session (`docs/perf-log.md`), with `K` = 0 (binary
only), 8, 16, 32 and linear-only:
- Add and cancel spread evenly over 100 levels: binary-only was fastest (add 10.4 ns vs 15.8 ns at
  `K` = 16; cancel 39.0 ns vs 50.3 ns). Every one-by-one check made it slower.
- The mixed stream, clustered near the middle: all five within about 9%, inside the noise.

So the one-by-one phase never paid for itself, and plain binary search is simpler. **Revisit with
real order flow in phase 4** (§13): if real lookups cluster near the best price much more tightly
than these benchmarks do, a hybrid could win, and it would be re-added only with numbers.

**Re-added (2026-10-21, phase 6 experiment E3):** on the real Nasdaq day, `K` = 4 gave 10% more
throughput than binary search only, with every latency guard equal or better; `K` = 8 and 16
were no faster and slowed adds (`docs/perf-log.md`, "E3"). Real orders cluster at the best, in
books thousands of levels deep.

**Known weakness: inserting a level deep in a deep book.** An insert shifts every better level along
by one, O(levels). It's rare (activity is near the best price), but in a book with thousands of
levels a single deep insert costs microseconds, a tail-latency spike. Found when a benchmark setup
that added 10,000 levels worst-first took minutes. **Measure in phase 3** (p99, p99.9). Candidate
fixes, simplest first, only if the numbers say it matters:
1. A **circular sorted array**: one pre-allocated ring, cheap to add at either end, a middle insert
   shifts toward the nearer end (at most half). Not `std::deque`, which allocates a block whenever
   it grows and gives blocks back when they empty, breaking rule 1.
2. A **chunked array**: levels in fixed-size blocks from a pool, so an insert only shifts within one
   block.
3. The **moving window** (§6.3).

### 6.2 Experiment: a plain tick array (one slot per possible price)

Like numbered lockers: the level for price `p` is entry `p − min_price`. **Finding a level needs no
search at all**, and cancel goes straight to it using the order's stored price.
- One array per side, with `max_price − min_price + 1` entries of 32 bytes, allocated once.
- **Finding the best price:** a **bitmap** with one bit per price, set when the level has orders.
  The next non-empty price is found 64 prices at a time with `std::countr_zero` ("count the zeros
  before the first set bit"). A second, smaller bitmap marks which 64-price blocks are non-empty,
  so even a sparse book skips empty stretches quickly.
- **Memory grows with the band width, not with the orders** (32 bytes per slot):

| Stock | Band | Slots per side | Memory per side |
|---|---|---|---|
| $100, 1¢ steps, ±10% | $90–$110 | 2,000 | 64 KB |
| $1,000, 1¢ steps, ±50% | $500–$1,500 | 100,000 | 3.2 MB |
| BRK.A ≈ $700,000, 1¢ steps, ±10% | $630k–$770k | 14,000,000 | 450 MB |

- **Narrow bands only.** `TickLevels` accepts bands of at most 65,536 ticks (2 MB per side) and
  asserts this at construction. All test, fuzzer and benchmark configs stay inside that, so the
  experiment is fully tested. It is **never** the default `FastBook` in phase 2.
- **What it answers:** on the same inputs, how much faster is a tick array than the sorted list
  (§9)? That number decides whether §6.3 is worth building.

### 6.3 Later, only if the numbers justify it: a moving window (phase 6)

A tick array covering only a slice of prices around the current best (say 4,096 slots), with
prices outside the slice kept in a sorted list. When the best price drifts near the edge, the
window **slides**: levels leaving the window move to the sorted list, levels entering move out of
it.

```
     far below        ┌──── window: tick array, no search ────┐       far above
  [sorted list] ◄──── │  ... $99.98 $99.99 $100.00 $100.01 ... │ ────► [sorted list]
                      └────────────────────────────────────────┘
```

- **Why it's attractive:** almost all activity is near the best price, so almost everything gets
  tick-array speed, for any band (including BRK.A).
- **Why it waits:**
  1. **Sliding causes rare, large pauses.** One slide can move hundreds of levels at once. The
     average stays low, but the slowest operations get much slower. That is the tail latency the
     README warns about (§4), and it can only be measured in phase 3.
  2. **A level can live in two places**, so there are more code paths and boundary bugs (an order
     exactly at the window edge, a slide during a sweep).
  3. **It may not be needed.** If §6.2 is not clearly faster than §6.1, the window adds complexity
     for nothing.
- **Condition to build it:** the tick array (§6.2) is clearly faster than the sorted list on the
  realistic benchmarks (mixed stream, aged book, large book). If so, it gets its own design section,
  its own fuzzer runs (including inputs that force slides), and a phase 3 tail-latency check.
- **Result (2026-10-17): condition met.** On the same machine in one session, the tick array was
  3–4× faster than the sorted list on the large book (cancel 31.8 vs 97.0 ns, add 18.1 vs 71.2 ns),
  1.5× on the mixed stream and 1.2× on the aged book (`docs/perf-log.md`). The moving window is
  therefore planned for **phase 6**, with its own design section first. `FastBook` keeps the sorted
  list until then, because it is the structure that works for any band today.

---

## 7. Matching

The algorithm is `design.md` §5–§7, unchanged. Only the storage it walks is different:
- Best opposite level from the level structure (§6); queue from `head`.
- For each resting order: same owner → `Canceled{SelfMatch}`, unlink, free the slot, continue.
  Otherwise trade `min(remaining, leaves)` at the level's price, update `leaves`, `filled` and the
  level's `total`, and on 0 pop it, remove it from the id table and free the slot.
- Empty level → remove it (A: clear its bitmap bit; B: erase from the vector).
- FOK's look-first count walks the queues the same way and skips own orders. It can't use the level's
  `total` directly, because that includes own orders. **[MEASURE]** later: whether it's worth also
  caching per-level totals that exclude self-match candidates (probably not).

---

## 8. Proving it correct

### 8.1 Typed tests
`FastBook` and the tick-array experiment (§6.2) are both added to `BookTypes`, so all 96 book and
exchange tests run on it unchanged.

### 8.2 Unit tests for each building block
| Part | Edge cases that must be covered |
|---|---|
| Order pool + free list | Take every slot; full pool; free and retake; last freed is first reused |
| Intrusive queue | Push to empty; unlink the only item, the head, the tail, the middle; pop until empty |
| Flat hash table | Insert, find, remove; a collision run; removing from the middle of a run; wrapping at the array end; ids that differ by the table size |
| Tick levels | Band edges; finding the next price across empty 64-price blocks; the last level emptied |
| Sorted levels | Insert at the best end, the deep end, the middle; binary search on both sides |

### 8.3 Differential fuzzer: `tools/nanobook_fuzz.cpp`
**Idea:** the reference book is trusted, so the right answer doesn't have to be known in advance.
Feed both books the same random inputs and compare.

1. **Seed** a random generator from `--seed S`, so the same seed always replays the same run.
2. **Generate** an input: a new order (any type, side, owner), a cancel or a modify. Ids increase
   with random gaps, like exchange ids.
3. **Feed it to both books** (each with its own `EventBuffer`) and compare the events element by
   element.
4. **Every N inputs**, also compare the books' state: best bid and ask, `level_qty` and
   `level_orders` for the top levels, `order_count`, `resting_ids`, and both books' invariants.
   Same events with drifted internal state is still a bug.
5. **On the first difference, stop and print:** the seed, the step number, the input, both event
   lists, and the last 20 inputs before it.

**Steered, not uniform.** Pure randomness is boring (most cancels would hit ids that never existed).
The generator deliberately makes interesting cases common:
- prices clustered near the middle, so orders actually trade, plus some far away
- cancels and modifies that mostly target orders still resting, some finished, a few never issued
- 3–5 owners, so self-matches happen often
- every order type, and modify both up and down, with and without a price change
- edge values: `max_qty`, band edges, invalid quantities and prices, a full book (small
  `max_orders` runs)

**Trusting the fuzzer itself:** first run it **reference vs. reference**, which must pass. Then run it
against a deliberately broken copy, which must fail. Both happen before the fast book exists.

**Runs:** CI does 100,000 operations on each of 3 seeds on every push (Release presets), and
`tests/test_fuzz.cpp` runs shorter fuzzes in every preset, including under ASan and UBSan. The
exit check does 10⁷+ on several seeds, locally.

**Replayable everywhere:** the generator takes its numbers straight from `std::mt19937_64`
(`rng() % n`), never from `std::uniform_int_distribution`. The standard fixes the generator's output
exactly, but not how a distribution uses it, so this is what makes a seed replay identically with
any compiler and standard library.

### 8.4 Zero-allocation test
- A separate test program links the counting `operator new` from `support/alloc_counter.cpp`. It's
  kept out of the sanitizer builds, so ASan's own allocation tracking stays intact. It runs in the
  Release presets.
- The test builds a fast book (allocations allowed: that's start-up), warms it up with a seeded
  random stream, then runs another 100,000 mixed operations and **expects exactly 0** allocations.
- Before the fast book exists, the tool itself is checked: code with one `new` must count 1, and
  code with none must count 0.

### 8.5 Invariants for the fast book
The same `invariants_hold()` contract, made real for the cached data:
- Every level's `total` and `count` match its queue. Its `head`/`tail`/`prev`/`next` links agree in
  both directions.
- Every queued order's `price` and `side` match its level. Its id is in the hash table, pointing
  back at its slot.
- Free slots + used slots = `max_orders`. No slot is both free and queued.
- Tick levels: a bitmap bit is set exactly when its level has orders. Sorted levels: prices
  strictly ordered, none empty.
- `resting_ids()` returns ids in ascending order, as the reference book does (it sorts; it's a test
  query, so that's fine).

---

## 9. Benchmarks

Same harness as phase 1 (`bench/bench_book.cpp`). Each fast-book variant is registered next to
`ReferenceBook`. Every change records a before/after table in `docs/perf-log.md` (rule 22).

| Benchmark | Status | Why |
|---|---|---|
| Add, cancel, match one, sweep 1/10/100, mixed | Exists | Direct comparison with the baseline |
| **Aged book** (`BM_Aged`) | Added in step 6 | Replays 200,000 steered random inputs untimed, then times 50 batches of 10,000 more on the same book, so memory is scattered as it would be after a trading day. Phase 1's fresh-heap numbers flatter `std::map` (report §8.2). |
| **Large book** (`BM_LargeCancel`, `BM_LargeAdd`) | Added in step 6 | 100,000 resting orders on 10,000 levels: bigger than the CPU's fast caches, so lookups really have to go to memory |
| Allocations per op | Exists | Must read 0.00 for the fast book |

Decided by these numbers: hash entry size (§5.1), order slot 48 vs 64 bytes (§4.1: 48 kept,
2026-10-18), the search
inside the sorted list (§6.1: binary search, decided 2026-10-17), and whether the moving window
(§6.3) is worth building: tick array (§6.2) vs. sorted list (§6.1).

---

## 10. Files

```
include/nanobook/fast/
  order.hpp            the 48-byte order slot, kNone and kFreeTag
  order_pool.hpp       pool + free list
  intrusive_queue.hpp  per-level FIFO links
  id_table.hpp         flat hash table: OrderId -> slot (replaced the step 4 temporary index)
  sorted_levels.hpp    main design (§6.1)
  tick_levels.hpp      experiment, narrow bands only (§6.2)
  fast_book.hpp        BasicFastBook<Levels>, FastBook
  level.hpp            the 32-byte Level (price, total, queue), shared by both level structures
src/fast/
  fast_book.cpp        explicit instantiation of both variants under the engine's flags
tests/fast/
  test_order_pool.cpp, test_intrusive_queue.cpp, test_id_table.cpp, test_levels.cpp
tests/test_alloc.cpp   zero-allocation test (separate program, Release only)
tests/test_fuzz.cpp    proves the fuzzer: no false alarms, catches planted bugs, replayable
support/alloc_counter  counting operator new, shared by tests and benchmarks
support/differential_fuzz.hpp  the fuzzer: generator, compare loop, report (header-only)
include/nanobook/core/names.hpp  enum names, shared by the test helpers and the fuzzer
tools/nanobook_fuzz.cpp        the fuzzer's command line
```

The fast book only includes `core/`. It must never include `reference/`, and vice versa.

---

## 11. Plan, one commit per step

| # | Step | Done when |
|---|---|---|
| 0 | This design note | Reviewed and committed (rule 23) |
| 1 | Zero-allocation test tool | Counts 1 for one `new`, 0 for none |
| 2 | Differential fuzzer + CI run | Passes reference vs. reference; catches a planted bug |
| 3 | Pool and intrusive queue, with unit tests | §8.2 cases pass |
| 4 | Fast book v1: pool + queues, with **temporary simple parts** for the id lookup and levels | All 96 typed tests pass, and the fuzzer passes (allocations not 0 yet: expected) |
| 5 | Flat hash table (§5.1), with unit tests, replacing the temporary lookup | Fuzzer passes; before/after cancel numbers |
| 6 | Sorted levels (§6.1, becomes `FastBook`) and the tick-array experiment (§6.2), with unit tests | Both pass everything; benchmarked against each other; result recorded, with the moving-window decision |
| 7 | Order layout (§4.1) | Before/after numbers |
| 8 | Exit check + phase 2 report | 10⁷+ fuzz on several seeds; zero-allocation test passes |

**Why step 4 uses temporary parts:** if everything is new at once and the fuzzer fails, the bug could
be anywhere. Build it correct first, then swap one part at a time. A failure always points at the
part just changed, and each swap gets its own before/after number.

---

## 12. Out of scope for phase 2

| Idea | Where it belongs |
|---|---|
| Price prediction, order-book imbalance models, optimal execution | **Not part of an order book** (that's a trading strategy). Optional "phase 8". |
| Kernel bypass, DPDK, special network cards, FPGAs | **Not in NanoBook**: there is no network (plan §1). An interview talking point only. |
| Moving-window tick array (§6.3) | Phase 6, only if the tick-array experiment is clearly faster |
| `[[likely]]`, `__builtin_expect`, manual prefetch | Phase 6, only with before/after numbers (rule 10) |
| `-march=native`, LTO, PGO | Phase 6 |
| Threads, ring buffers | Phase 5 |
| ITCH parsing | Phase 4 |
| Tail latency (p99, p99.9) | Phase 3 (`rdtsc` harness) |

---

## 13. Revisit with real data (phase 4)

Decisions made on synthetic benchmarks that real Nasdaq order flow (the ITCH replay) could change.
Each is re-measured once phase 4 can feed a real trading day through the books.

| Decision | Made on | What real data could show | If it does |
|---|---|---|---|
| Plain binary search in the sorted list; the hybrid dropped (§6.1) | Evenly spread add/cancel and a clustered mixed stream | Lookups clustering near the best price much more tightly than our benchmarks | Re-measure a hybrid (one-by-one checks near the best, then binary search) |
| Sorted list as the default; tick array an experiment (§6) | Synthetic benchmarks | How wide real price ranges are per stock, and how deep real books get | Decide the moving window (§6.3) |
| Deep inserts are rare (§6.1 known weakness) | Assumption | How often a brand-new price appears deep in a deep book | Pick a fix from the §6.1 candidate list |

**Answered by phase 4 (2026-10-20, one real Nasdaq day; `docs/phase4-report.md` §10,
`docs/itch-design.md` §9.1):**
- **Clustering near the best: yes.** 48% of new orders land at or improve the best price, 93%
  within $1, and 35% of new levels are created at the best end of the array. The hybrid search is
  worth re-measuring on real flow (phase 6). **Re-measured (phase 6 E3): `K` = 4 kept, 10% more
  throughput** (§6.1).
- **Price ranges: as wide as the spec allows.** Stub quotes stretch a stock's range from $0.0001
  to $199,999.99 (AAPL at the close: 3,053 bid and 2,602 ask levels; its peak, 42,774 resting
  orders). A fixed tick array is ruled out (about 489,000 GB for the day); a window of tick slots
  near the best plus an overflow structure fits the data (§6.3, phase 6).
- **Deep inserts: rare but real.** 0.02% of 59 million level creations shift 1,024 or more levels
  (the deepest 2,048–4,095); about 40 GB of level shifting over the day. A fix from the §6.1 list,
  with before/after numbers, is phase 6's.

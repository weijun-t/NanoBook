# Phase 4 design: replaying a real Nasdaq day (ITCH 5.0)

> **Status:** approved (2026-10-20). Changes from here on are made by an edit plus a note in the
> commit message. Decided (§13): the 30 Jan 2019 day, feed operations on both books, a pre-scan to
> size the books, 1 GiB windows, histogram plus a raw subset, an encoder for the golden test, the
> file downloaded now on WSL2. Items marked **[MEASURE]** are settled by a measurement in the step
> named. Step 1 (2026-10-20): the file's size, framing and contents measured (§2, §3.1, §3.5, §9).
> Step 2: the assembly check and the move to Clang 19 (§5).
> Step 3: the feed operations as built: inputs, results, check order, `structure_holds()` (§6.1).
> Step 4a: sizing as built (§7); step 4 split into 4a and 4b (§12). Step 4b: the crossed-book
> rule decided from the real day (§8.1), the first full-day replay's results (§8.5). Step 5: the
> differential replay as built and its result (§8.2). Step 6: timing as built and Q5 changed
> (§9, §13), TickFastBook can't hold the real day (§9), the flow analysis (§9.1); step 6 split
> into 6a, 6b, 6c (§12). Step 6c: the timed runs (§12; numbers in `docs/perf-log.md`). Step 7a:
> the snapshot and flow CSVs and the two plot scripts (§10); step 7 split into 7a and 7b (§12).
> Step 7b: the phase 4 report; phase 4 complete except step 8, the bare-metal numbers. Phase 3 step 6 (the bare-metal
> run) is still open; phase 4's own published numbers come from the same Ubuntu session.

Phases 1–3 built **the core of an exchange** and fed it orders we made up. Phase 4 plays the other
side: **a trading firm watching Nasdaq on one real trading day**. It rebuilds every stock's book
from Nasdaq's own record of that day, and measures how fast our order book does it.

---

## 0. Background in plain words

### 0.1 Orders and the order book
An **order** says: *"buy (or sell) this many shares of this stock, at this price or better"*.
For example: "buy 100 Apple at most $190.05".

A stock's **order book** is the list of orders that are **waiting**, which nobody has traded with
yet. Buyers (bids) are sorted from the highest price down; sellers (asks) from the lowest price
up. At one price, orders queue first come, first served.

| Buyers (bids) | | Sellers (asks) | |
|---|---|---|---|
| **$190.04**: 100 (Ann), 50 (Bob) | ← best bid | **$190.05**: 200 (Cat) | ← best ask |
| $190.03: 300 (Dan) | | $190.06: 80 (Eve) |

Nothing trades here: the most a buyer will pay ($190.04) is less than the least a seller will
accept ($190.05). The book holds **waiting orders, not trades**. When two orders trade, the shares
that traded **leave** the book. A list of past trades is a separate record ("the tape").

### 0.2 The matching engine
The **matching engine** is the set of rules that decides **when trades happen**. A new order
"buy 250 at up to $190.05" arrives. It trades 200 with Cat (best price first, then earliest
arrival: **price-time priority**). Cat's order is used up and removed. The other 50 have no seller
left at $190.05 or below, so they wait in the book as a new bid.

### 0.3 What phases 1–3 built: the core of an exchange

| Layer | In the code | Job |
|---|---|---|
| **Order book** | Price levels, queues, order pool, id lookup | Store the waiting orders, sorted |
| **Matching engine** | `handle(NewOrder / Cancel / Modify)` in each book | Decide trades by price-time priority |
| **`Exchange`** | `include/nanobook/exchange.hpp` | One book per stock; give each order an id; send it to its stock's book |

A real exchange also has network connections, account and risk checks, fees and settlement. Those
are left out on purpose (README §1). NanoBook is the part that **holds the orders and decides
the trades**, which is the part that has to be fast.

### 0.4 Phase 4: the trading firm's side
A trading firm doesn't run Nasdaq's matching. It needs to **see** Nasdaq's book (who waits at
which price) to decide its own trades. Nasdaq doesn't send a picture of the whole book every
moment. It sends **only the changes**, one message each. That's the ITCH feed:

| Time | Message | Meaning |
|---|---|---|
| 10:00:01 | `A` order 501: sell 70 @ $190.05 | A new waiting order joined the book |
| 10:00:02 | `E` order 501: 30 executed | 30 of it traded (Nasdaq did the matching) |
| 10:00:03 | `D` order 501 | The rest was cancelled; it left the book |

So the firm keeps **its own copy** of each book. It starts empty at the beginning of the day and
applies every change in order. Then the copy equals Nasdaq's real book at every moment. That's
**rebuilding the book**. It's like a bank account: the bank sends each transaction, not your
balance, and applying them all in order always gives the balance. Or like replaying a recorded
chess game on your own board: you decide no moves, you only apply them.

The ITCH file is **one real day of these change messages, for every stock**: hundreds of
millions of them, read from a file on disk. Nothing is live, and nothing connects to Nasdaq.

### 0.5 Why phase 4 doesn't use our matching
The file records **outcomes** of Nasdaq's matching, so it can't drive ours:
- **The orders that caused trades aren't in it.** "Order 501: 30 executed" doesn't include the
  buyer's order, so our engine would have nothing to match.
- **Some orders are invisible.** Hidden orders trade on Nasdaq but are never added to the
  displayed book, so our engine couldn't reproduce those trades.
- **The ids are already chosen.** Later messages refer to Nasdaq's ids (order 501), so our book
  must use them instead of numbering orders 1, 2, 3 as our `Exchange` does.

Matching on this data would give a different, made-up day. The only faithful thing to do is
**apply** each change. So the book gets a few new, simpler inputs: "add this order without
matching", "take N shares off order X", "remove order X" (§6.1). They sit on top of **the same
order book**: pool, price levels, queues and id lookup.

### 0.6 What gets benchmarked

| | Phases 1–3 | Phase 4 |
|---|---|---|
| NanoBook plays | **The exchange** (it decides trades) | **A trading firm** (it copies Nasdaq's book) |
| Input | Orders we made up | Nasdaq's real record of one day |
| Uses our matching | **Yes** | **No**: Nasdaq already matched |
| Uses our order book | Yes | **Yes, the same one** |
| Answers | How fast is our exchange? (phase 3's nine scenarios, e.g. about 11 ns per add on the fast book, but 1.41 µs at p50 for a deep insert; `docs/perf-log.md`) | How fast can our book follow a real day, and what does real order flow do to its data structures? |

Phase 4 doesn't replace phase 3's numbers. The matching benchmarks stay phase 3's, on made-up
orders, because real data can't drive matching faithfully. Phase 4 adds the second half: **our
order book on real data**. It also answers the questions phase 2 left open (fast-book design §13):
do real orders cluster near the best price, how deep do real books get, and is the slow deep
insert rare or common? Those answers decide what phase 6 optimises.

---

## 1. Goal and exit criteria

**Goal:** `nanobook_replay` reads a full-day ITCH 5.0 file, rebuilds the book of every stock, and
prints the top levels of one stock plus stats (README §7.4).

**Phase 4 is done when** (README roadmap):
1. A **full-day replay completes**.
2. The **consistency checks pass** (rule 13, §8): no negative quantities, every executed or
   cancelled order existed, no crossed book outside auctions.
3. **Messages/sec and per-message latency are reported** (§9). On WSL2 they carry the NOT FOR
   PUBLISHING banner. The published versions come from bare metal (rule 20).

Also built: `plot_depth.py` (§10), and a golden replay test in CI (§8.3).

---

## 2. The data

Nasdaq publishes whole sample days at `emi.nasdaq.com/ITCH/Nasdaq ITCH/` (checked 2026-10-20).
For example `01302019.NASDAQ_ITCH50.gz` is **4.76 GB compressed** (4,764,426,091 bytes) and
**11.25 GB uncompressed** (11,245,883,092 bytes, 2.36 times bigger; measured in step 1). The
download was checked against the server's byte count and the gzip checksum, and the extracted
file against a fresh decompression (identical). Nasdaq's `.md5sum` files are listed but not
served (HTTP 404).

- **Where it lives:** `data/itch/` in the repo folder, **ignored by git** (a new `.gitignore`
  line). It's far too big for git, and it's Nasdaq's data, not ours to republish. The extracted
  file is `data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50` (the Windows unzip tool made
  the folder).
- **Disk:** WSL2's disk is a file on C: that **grows but doesn't shrink on its own** when files
  are deleted. So a 15 GB download today makes C: 15 GB fuller, and that matters when C: is
  shrunk for Ubuntu. Options: download it on the Ubuntu side later, or compact the WSL disk after
  deleting it.
- **Memory:** WSL2 sees **15 GB of RAM** here. The 11.25 GB file only just fits on its own (the
  step 1 scan, which mapped all of it, peaked at 10.7 GB resident), so not next to the books.
  That's why we read it in windows (§4).

---

## 3. The format

### 3.1 Framing
The file is a sequence of messages, each **preceded by a 2-byte big-endian length** (the spec
PDF covers the messages, not the file framing). **Confirmed in step 1:** the first bytes are
`00 0c` (length 12) then `S` ... `O` (System Event "Start of Messages"); walking the whole file by
the lengths gives 368,366,634 messages, the last one ending exactly at the end of the file, and
every message's length matches its type's length in the spec.

### 3.2 Every message starts with the same 11 bytes

| Offset | Length | Field | Notes |
|---|---|---|---|
| 0 | 1 | Message type | One letter, e.g. `A` |
| 1 | 2 | Stock locate | A small number per stock, **assigned fresh each day from 1**, "intended as an array index" (spec). 0 = not about one stock |
| 3 | 2 | Tracking number | Nasdaq internal. Ignored |
| 5 | 6 | Timestamp | Nanoseconds since midnight, a **6-byte** integer |

**Data types (spec):** all integers are **big-endian** and unsigned. Text is ASCII, padded with
spaces. Prices are integers with 4 implied decimal places: `Price(4)` 1,905,000 = $190.5000.
The largest price is 200,000.0000.

### 3.3 The messages that change the book

| Type | Name | Length | Fields after the header | Effect on our book |
|---|---|---|---|---|
| `A` | Add Order | 36 | order ref (8), side `B`/`S` (1), shares (4), stock (8), price (4) | Add a resting order at the back of its level |
| `F` | Add Order with MPID | 40 | as `A`, plus attribution (4) | Same as `A` |
| `E` | Order Executed | 31 | order ref (8), executed shares (4), match number (8) | Reduce the order by that many; remove it at 0 |
| `C` | Order Executed With Price | 36 | as `E`, plus printable (1), execution price (4) | Same as `E` (the price is for trade reports) |
| `X` | Order Cancel | 23 | order ref (8), cancelled shares (4) | Reduce the order, **keeping its place in the queue** |
| `D` | Order Delete | 19 | order ref (8) | Remove the order |
| `U` | Order Replace | 35 | old ref (8), new ref (8), shares (4), price (4) | Remove the old order; add the new one, **same side**, at the back of the queue (spec: side can't change) |

The spec says order refs are **unique for the whole day**, and that `E`, `C` and `X` are
**cumulative**: an order can be reduced several times.

### 3.4 The messages we read but that don't change the book

| Type | Name | What we use it for |
|---|---|---|
| `S` | System Event | Start/end of messages, market open (`Q`) and close (`M`) times |
| `R` | Stock Directory | Stock locate → symbol name ("AAPL"), so `--symbol AAPL` works |
| `H` | Stock Trading Action | Per-stock state: Halted, Paused, Quotation only, Trading. Needed for the crossed-book check (§8.1) |
| `P`, `Q` | Trade (non-displayed), Cross Trade | Counted in stats only. The spec says they don't affect the book |

All other types (`Y`, `L`, `V`, `W`, `K`, `J`, `h`, `I`, `N`, `O`, `B`) are skipped using the
length prefix and counted.

### 3.5 What the real day contains (step 1)
Counted with a throwaway scanner (outside the repo) that walks the file by the length prefix. The
step 4 replay counts again, as a cross-check.

| Type | Messages | Share | | Type | Messages | Share |
|---|---|---|---|---|---|---|
| `A` Add | 162,970,455 | 44.24% | | `I` Imbalance (NOII) | 3,684,511 | 1.00% |
| `D` Delete | 158,273,361 | 42.97% | | `P` Trade (non-displayed) | 1,326,184 | 0.36% |
| `U` Replace | 27,222,746 | 7.39% | | `L` Participant position | 193,769 | 0.05% |
| `E` Executed | 8,096,995 | 2.20% | | `Q` Cross trade | 17,430 | 0.00% |
| `X` Cancel (partial) | 4,669,874 | 1.27% | | `Y` Reg SHO | 8,821 | 0.00% |
| `F` Add with MPID | 1,725,898 | 0.47% | | `H` Trading action | 8,805 | 0.00% |
| `C` Executed with price | 158,886 | 0.04% | | `R` Stock directory | 8,714 | 0.00% |
| | | | | `B`, `J`, `S`, `V` | 116, 62, 6, 1 | 0.00% |
| **All** | **368,366,634** | | | | | |

What it tells us:
- **98.6% of messages change the book** (`A`, `D`, `U`, `E`, `X`, `F`, `C`). The book is the
  work; everything else is a rounding error.
- **Most orders are cancelled, not traded.** There are about 19 deletes for every execution
  (158.3 M `D` against 8.3 M `E` + `C`). Real flow is add, wait, delete; a trade is rare. So add,
  delete and replace speed matter most here, not matching.
- **8,713 stocks**, locates 1 to 8,713 with no gaps (Apple is 14). One directory message is
  **re-sent** during the day for a stock already listed (`CFG-D`, locate 1335, at 05:16:01), so
  the builder treats `R` for a known locate as an update, never as a second book.
- **Order refs go up to 329,176,641** while 191.9 M orders are added (`A` + `F` + `U`): not
  every number is used, but they stay below about 330 M. Relevant to fast-book design §5.2 (one
  table entry per possible id, rejected for synthetic ids); a phase 6 question, not phase 4's.
- **The day's clock:** start of messages 03:03:59, system hours 04:00, market open 09:30, close
  16:00, end of system hours 20:00, end of messages 20:05 (nanoseconds since midnight, New York
  time). Types `W`, `K`, `h`, `N` and `O` never appear in this file.
- **A plain walk of the whole file takes about 6 seconds** (one run, file already in memory,
  WSL2, with another program reading the disk at the same time: not a benchmark). Enough to know
  the pre-scan (§7) costs seconds, not minutes.

---

## 4. Reading the file

- **mmap:** the kernel maps the file into our address space, so the bytes are read in place, with
  no `read()` copies into buffers.
- **Pre-fault before timing (README §7.2):** touch every page first so the timed replay
  measures the book, not the disk.
- **Windows, because of §2's memory limit:** pre-fault a **window** (say 1 GiB, aligned to whole
  messages), replay it with the clock running, then release it with `madvise(MADV_DONTNEED)`
  and move on. Only the time spent replaying a window counts. On bare metal with more RAM, one
  window can be the whole file. The window size is a command-line option.

Cost: one pass over the file. Pre-faulting is cold path.

---

## 5. Parsing

- **No `reinterpret_cast` of the buffer onto a struct.** That's undefined behaviour (strict
  aliasing, alignment; README §4). Each field is read with a small helper: `std::memcpy` into an
  integer, then `std::byteswap`. **Checked in step 2** (`-O2` assembly, GCC 14 and Clang 19): a
  4- or 8-byte field is exactly one load plus one `bswap`. The 6-byte timestamp is read as 2 bytes
  plus 4 bytes and combined: two loads, two swaps, a shift and an OR (7 instructions).
- **"Zero-copy"** means the message bytes are never copied into a buffer or a string. Each field
  goes straight from the mapped file into a register, in a small decoded struct (`AddOrder`,
  `OrderExecuted`, ...).
- **Dispatch:** a `switch` on the type letter (rule 4: no virtual calls). The parser calls a
  handler that's a template parameter, so a test handler, a counting handler and the book builder
  all use the same parser at zero cost.
- **Bad input:** a length that runs past the end of the file, or doesn't match the type's fixed
  length, is reported and stops the replay. The parser never reads past the buffer. Errors use
  `std::expected` (cold path). That needs **Clang 19**: with GCC 14's standard library, Clang 18
  doesn't get `std::expected` at all (it reports C++20 concepts as `201907`, and the library wants
  `202002`), so the project moved from Clang 18 to 19 before step 2.

---

## 6. Building the books

### 6.1 New book operations
Our books' `NewOrder` *matches*, and `Modify` uses FIX's "new total" rule (design §7). Neither is
what a feed means. So both books get **four feed operations**, written down as a second concept,
`FeedBook`, next to `OrderBook` (`include/nanobook/core/order_book.hpp`). As built in step 3:

| Input (`core/messages.hpp`) | From | Does | Fails, in this order (book unchanged) |
|---|---|---|---|
| `apply(FeedAdd{id, side, price, qty})` | `A`, `F` | Rest at the back of its level, **without matching**, even if the price crosses (Nasdaq already matched) | `InvalidQty`, `InvalidPrice`, `DuplicateOrder`, `BookFull` |
| `apply(FeedReduce{id, qty})` | `E`, `C`, `X` | Take `qty` off, keeping the order's place; remove it at 0 | `UnknownOrder`, `InvalidQty`, `ReduceTooLarge` |
| `apply(FeedRemove{id})` | `D` | Remove the order | `UnknownOrder` |
| `apply(FeedReplace{old_id, new_id, price, qty})` | `U` | Remove the old order, rest the new one on the **same side**, at the back of its level | `UnknownOrder`, `InvalidQty`, `InvalidPrice`, `DuplicateOrder` |

- Each returns a `FeedResult`: `Ok`, or why it failed. **Every check comes before any change**,
  so a failed input leaves the book exactly as it was, and both books must report the same
  result. Those results are what step 4's consistency checks count (§8.1).
- **No events, no matching, no owner.** A reduce shrinks the order's total; `filled` stays 0,
  because a copy never uses `Modify`'s "total including what traded" rule.
- **Ids needn't increase**, unlike on our exchange (design §4): a copy takes them as the feed
  gives them. A book is used either through `handle()` (as our exchange) or through `apply()`
  (as a copy), never both.
- **A crossed book is allowed.** `invariants_hold()` still means "a valid exchange book" (never
  crossed). The new `structure_holds()` is every invariant except "not crossed", which is what a
  copy must keep; whether Nasdaq's book is ever crossed is step 4's measurement (§8.1).

Both the **reference book** and the **fast book** get them. That's what lets us run the
differential check on real data (§8.2), the same way phase 2 proved the fast book. Step 3 already
runs it on 100,000 random feed inputs (`tests/test_feed.cpp`).

The fast book reuses its pool, levels, intrusive queues and id table unchanged. Only the four
entry points are new; a replace keeps the order's pool slot, as `Modify` does. The tick-array
book needs a price band per stock (§7).

### 6.2 One book per stock
The stock locate is already an array index (spec), so `books[locate]` finds the book with one
load, like our `SymbolId`. Each `E`/`C`/`X`/`D`/`U` also carries the locate, so the id lookup
happens inside that one stock's book.

---

## 7. Sizing every book before the replay (rule 1: no allocation while replaying)

A book needs its limits at construction (`BookConfig`): the largest number of resting orders,
and for the tick array, the price band. Real stocks differ hugely (a $2 stock vs a $700 stock).

**Proposal: a pre-scan.** One fast pass over the file, before the replay, that only counts. For
each stock it records:
- the **peak** number of orders resting at once,
- the lowest and highest price seen,
- the largest order size.

The books are built from these, plus headroom. The pre-scan is cold path, and its time is reported
separately. It also gives the first facts about real flow that fast-book design §13 asked for:
how wide real price ranges are, and how deep books get.

**As built (step 4a).** Counting (`include/nanobook/itch/day_stats.hpp`) and sizing
(`sizing.hpp`) are separate, so the counts can come from another day:
- **Stats are keyed by stock symbol, not locate.** The spec renumbers locates every day, so a
  later version can size today's books from **yesterday's** stats with bigger margins, which is
  what a real firm would have to do. Step 4 uses the same day's stats.
- **Margins in whole percentages** (exact; `1.1` has no exact `double`): room for 125% of the
  peak plus 16 orders; the price range widened by 10% each way.
- **A stock with no stats** (a new listing, or no orders that day) gets a small default book that
  accepts any price. Locate 0 and gaps in the directory get a tiny book that is never used,
  because **only a Stock Directory (`R`) message creates a stock**: the builder counts an order
  for an unnamed locate as unknown.
- **`max_qty` is the largest share count ITCH can carry**: a copy accepts whatever Nasdaq did.
- **Overflow is counted, never silent**: a stock that outgrows its book gets `BookFull` (or
  `InvalidPrice`) from the book, and the builder counts it.
- **The tick-array book can't take the any-price default** (2 billion prices x 32 bytes = 64 GB),
  so it is only used where every stock has stats; `FastBook` replays the real day.

---

## 8. Proving it right

### 8.1 Consistency checks (rule 13), after every message in check mode
- **No negative quantities:** a `reduce` larger than what's left fails.
- **Every executed, cancelled, deleted or replaced order existed:** an unknown ref fails.
- **No new order reuses a live ref.**
- **No crossed book outside auctions:** after each message, for that stock, best bid < best ask.
  When this is allowed, though, was not obvious. The spec has per-stock halts, pauses and
  quotation periods (`H`) and market-wide open/close times (`S`). **Decided from the real day
  (step 4b, §8.5): a stock's book may be crossed only while its trading state is not `T`**
  (halted `H`, paused `P`, quotation only `Q`: the periods that end in a reopening auction). All
  8 crossings on 30 Jan 2019 were in `P` or `Q`. The builder counts crossings while `T` as
  `crossed_while_trading`, and `nanobook_replay --check` fails if it isn't 0.
- Every book's `structure_holds()` (every invariant except "not crossed"), every N order
  messages (`--structure-every N`).

Check mode is slower. It's on for correctness runs and off for timed runs.

### 8.2 Differential replay
Replay the day into the reference book and the fast book side by side, and compare the books for
every stock after each message, or every N messages, for speed. This is phase 2's fuzzer, fed
real flow instead of random flow.

**As built (step 5):** `nanobook_replay --differential` (`support/itch_replay_comparison.hpp`).
- After **every** order message, a quick check of the stock it touched: order count, best bid and
  ask, and their quantities.
- A **full** check of that stock every million order messages, and of **every** stock at each
  System Event (start of day, market open and close, ...) and at the end. A full check compares
  every level (`depth()`: the reference book adds each total up from its orders, the fast book
  uses its cached total, so the cache is checked too), every queue's order, and every order's
  remaining shares.
- The two builders' statistics (every result, every crossing) must match at the end.

**Result on 30 Jan 2019 (2026-10-20): 0 mismatches** in 363,118,215 quick checks and 61,354
full checks (363 single stocks, plus all 8,713 stocks 7 times). The fast book equals the
reference book after every one of the day's order messages.

### 8.3 Golden replay test (CI, every commit)
A small ITCH byte file, and the expected final books and stats, both checked in. The test replays
it and compares. **Proposal:** the bytes are **written by a small test encoder** (made-up stocks
and orders covering every message type, including the tricky ones: partial executes, replace,
cancel to zero), not cut from Nasdaq's file. It's small, readable, and has no question about
republishing Nasdaq's data. A local-only test can also replay a real slice when the file is
present.

### 8.4 Parser tests
Each message type decoded from hand-made bytes (big-endian values, the 6-byte timestamp). Plus:
lengths that are too short, too long, or past the end of the file are all rejected. That's the
groundwork for the optional libFuzzer target (README §7.1).

### 8.5 The first full-day replay (step 4b, 2026-10-20)
`nanobook_replay data/itch/01302019.NASDAQ_ITCH50/01302019.NASDAQ_ITCH50 --check
--structure-every 100000`, `gcc-release`, WSL2. FastBook books sized by the counting pass (§7).

| Check | Result |
|---|---|
| Messages replayed | **368,366,634**, every type's count equal to the census (§3.5) |
| Order messages applied | **363,118,215, all `Ok`**: 0 unknown orders, 0 reduce too large, 0 duplicates, 0 bad prices or quantities, 0 `BookFull` |
| Order messages for an unnamed locate / bad input | 0 / 0 |
| Crossed books | 8 times (663 order messages left a book crossed), **all while the stock was paused (`P`) or quotation-only (`Q`)**: EYEN at 07:27 (`Q`, pre-market), DLPN, PHUN and SXTC between 09:46 and 11:44 (`P`). **0 while trading (`T`)** |
| Book structure | 3,631 checks during the day, 0 failures |
| Reference book vs fast book (step 5, `--differential`) | **0 mismatches** in 363,118,215 quick and 61,354 full checks (§8.2) |
| Orders resting after the last message | 0: Nasdaq deletes every remaining order after the close, and our copy ends empty too |
| Sizing | room for 2,522,876 resting orders in all; no stock outgrew its book |
| Most orders resting at once | AAPL 42,774, AMZN 39,028, MSFT 22,116, FB 20,986, NVDA 18,558 |
| At the 16:00 close (`--until 16:00:00`) | 1,579,011 orders resting; AAPL best bid $165.24, best ask $165.27 |

What it means: the parser, the feed operations and the books agree with every one of Nasdaq's
messages for a whole day, with no exception to explain away. Times, for orientation only
(disk reads included, machine not checked to be idle, not a benchmark): counting pass about 31 to 40 s,
building the books 0.2 s, replay with checks about 53 s; peak memory 11 GB, almost all of it the
file's pages. Timing is step 6.

---

## 9. What gets measured

- **Messages/sec:** the timed windows only (§4), the fast book with checks off. Reported next to
  the pre-scan time and the message counts per type.
- **Per-message latency:** the `rdtsc` timer from phase 3 around each message's book update,
  tagged by message type (add, execute, cancel, delete, replace).
- **Memory for the samples:** phase 3 kept every raw sample (5 MB per million). A full day is
  368 million messages (§3.5), so raw samples for all of them would take about 1.8 GB.
  **Decided (step 6, replacing Q5's histogram):** raw samples of **every 8th order message**,
  about 45 million (227 MB), in phase 3's Recorder unchanged. Exact percentiles (p99.99 rests on
  ~4,500 samples), and the 7 untimed messages in 8 run at full speed (the timer's fences cost
  ~12 ns and stop the CPU overlapping a timed message with the next).
- Environment block and NOT FOR PUBLISHING banner exactly as phase 3 (`support/environment.hpp`).

**As built (step 6):** `nanobook_replay FILE --throughput --latency [--every 8] [--window-mb 1024]
[--core C] [--csv FILE.csv]` (`support/itch_timing.hpp`).
- `replay_in_windows()`: each window's pages are loaded (MADV_WILLNEED, then one read per 4 KiB
  page) before the clock starts, and released (MADV_DONTNEED) after, so the disk is not timed
  and memory stays near one window. The counting pass runs in windows too.
- **Throughput:** `ParseOnly` (decodes every message, keeps a checksum so the decoding can't be
  optimised away) gives the parser's time alone; the same windows through `BookBuilder<FastBook>`
  give parser + books; the difference is the books' share. Checks off.
- **Latency:** `TimedOrders` times every Nth order message, tagged A, F -> add; E, C -> match;
  D -> cancel; X -> modify; U -> replace (a new phase 3 tag). A sample covers the builder's and
  the book's work, not the parsing.
- **FastBook only.** TickFastBook can't hold the real day: with step 4's sizing its tick arrays
  would need about **489,000 GB** (2,123 stocks with no stats get the any-price default, 64 GB
  each; of the rest, 4,032 need over 100 MB, the largest 128 GB), because real price ranges run
  from about $0.0001 to $200,000. It stays as the measured lookup baseline for phase 6's
  moving-window design (fast-book design §6.3).

### 9.1 What real flow does to the price levels (step 6, `--flow`, untimed)
`support/itch_flow.hpp` follows every order and, for each price level created or removed, counts
the levels it shifted in FastBook's sorted array (the levels better than it; fast-book design
§6.1), and for each new order its distance from its side's best price. 30 Jan 2019, all 8,713
stocks:

| Levels shifted by one creation | Share of 59,027,714 creations | Cumulative |
|---|---|---|
| 0 (at or beyond the best) | 34.69% | 34.69% |
| 1 | 9.83% | 44.52% |
| 2-3 | 11.07% | 55.59% |
| 4-15 | 25.71% | 81.31% |
| 16-63 | 16.53% | 97.84% |
| 64-1,023 | 2.14% | 99.98% |
| 1,024-4,095 | 0.02% (12,721) | 100.00% |

Removals (59,027,714, the same count: every book ends empty) look the same: 31.93% shift 0,
92.87% at most 31, 1,162 shift 1,024 or more. In all, 641 million levels shifted by creations
and 613 million by removals: about **40 GB copied over the day**, 10.9 levels (350 bytes) per
creation on average.

| New resting order's distance from its side's best | Share of 191,919,099 |
|---|---|
| side was empty | 0.05% |
| improves the best | 10.49% |
| at the best | 37.35% |
| behind by < $0.01 | 0.15% |
| < $0.10 | 30.65% |
| < $1 | 14.18% |
| < $10 | 6.60% |
| < $100 | 0.50% |
| $100 or more | 0.04% |

What it answers (fast-book design §13):
- **Deep inserts are rare but real:** 0.02% of creations shift 1,024+ levels. Phase 3's
  `deep_insert` (thousands of levels every time) was a worst case, not typical flow.
- **Activity clusters near the best**: 48% of new orders at or improving it, 93% within $1. But
  the far tail is what makes a fixed tick band impossible: a few stub quotes stretch every range.
  That's the case for a window of tick slots near the best plus an overflow structure for the
  rest (fast-book design §6.3).
- **Price ranges are as wide as the spec allows**, which rules out TickFastBook as it is (above).

---

## 10. Tools and plots

| Tool | Does |
|---|---|
| `nanobook_replay FILE [--symbol AAPL] [--depth 10] [--until HH:MM:SS] [--check] [--structure-every N] [--differential]` (built in steps 4b and 5); step 6 adds `[--window-mb N] [--book reference\|sorted\|tick] [--snapshot-csv out.csv] [--csv out.csv]` | Counting pass, sizing, replay; prints messages by type, every order message's result, crossed books, the biggest books, and the top N levels of one stock. Snapshot CSV for depth plots |
| `scripts/plot_depth.py SNAPSHOT.csv [--levels 50]` (step 7a) | Cumulative quantity by price, bids against asks, from `nanobook_replay --symbol S --until T --snapshot-csv FILE`. The eyeball check that the rebuilt book looks like a market. Only the N levels nearest the best price are drawn (stub quotes would stretch the axis from cents to $200,000); the subtitle says how far the rest reach |
| `scripts/plot_phase4.py` (step 7a) | The phase 4 report's charts: latency per message kind against phase 3 (`phase4_latency.png`), and the flow analysis (`phase4_flow.png`, from `nanobook_replay --flow --csv FILE`) |

Same rules as phase 3: tools outside the library, public API only, all printing after the timed
region (README §7.4).

---

## 11. Files

| File | What |
|---|---|
| `include/nanobook/itch/messages.hpp` | The decoded message structs and type letters |
| `include/nanobook/itch/parser.hpp` | Field readers (`memcpy` + `byteswap`), the `switch`, length checks |
| `include/nanobook/itch/file.hpp`, `src/itch/file.cpp` | mmap, windows, pre-fault |
| `include/nanobook/itch/book_builder.hpp` | Messages → feed operations on `books[locate]`, stats |
| Reference and fast books | The four feed operations (§6.1), a `FeedBook` concept |
| `support/itch_encoder.hpp` | Test-only: writes ITCH bytes (golden test, parser tests) |
| `tools/nanobook_replay.cpp` | The CLI |
| `scripts/plot_depth.py` | The plot |
| `tests/itch/...` | Parser, feed operations, builder, golden replay |

---

## 12. Plan, one commit per step

| Step | What | Needs the data file? |
|---|---|---|
| 0 | This design note, decisions recorded | No |
| 1 ✓ | Download one day; confirm the framing; count messages per type with a tiny throwaway scan (the uncompressed size, the message count). Done 2026-10-20 (§3.5) | Yes |
| 2 ✓ | Parser + test encoder + parser tests. Done 2026-10-20, after moving Clang 18 → 19 (§5) | No |
| 3 ✓ | Feed operations on both books + tests (incl. the zero-allocation test). Done 2026-10-20 (§6.1) | No |
| 4a ✓ | Counting, sizing (§7), book builder, golden replay test (§8.3). Done 2026-10-20 | No |
| 4b ✓ | `nanobook_replay` (minimal); first full-day replay with checks; the crossed-book rule (§8.1). Done 2026-10-20 (§8.5) | Yes |
| 5 ✓ | Differential replay reference vs fast on the full day. Done 2026-10-20: 0 mismatches (§8.2) | Yes |
| 6a ✓ | Timing in pre-loaded windows: `--throughput` (parse alone vs parse + FastBook), `--latency` (every 8th order message by kind), CSV (§9). Built 2026-10-20 | No |
| 6b ✓ | Real-flow analysis `--flow`: levels shifted per level created or removed, distance from best (§9.1). Done 2026-10-20 | Yes |
| 6c ✓ | The timed runs on an idle laptop, two rounds, recorded in `docs/perf-log.md` (WSL2, not for publishing). Done 2026-10-20: 104 ns per message (parser 3%), p50 105 ns, p99 429 ns | Yes |
| 7a ✓ | `--snapshot-csv` and `--flow --csv` in `nanobook_replay`; `plot_depth.py`, `plot_phase4.py` (§10). Built 2026-10-20 | No |
| 7b ✓ | From a clean commit: the snapshot and flow CSVs, the charts, the phase 4 report (`docs/phase4-report.md`, `.pdf`); fast-book design §13 revisit list answered from real data; README status. Done 2026-10-20 | Yes |
| 8 | Published numbers: with phase 3 step 6, on bare metal | Yes |

---

## 13. Decisions (approved 2026-10-20)

| # | Question | Decision | Why |
|---|---|---|---|
| Q1 | Which day? (§2) | `01302019.NASDAQ_ITCH50.gz` (Wednesday 30 January 2019, 4.76 GB compressed) | A full mid-week trading day, mid-sized among the files listed (3.5 to 5.6 GB for the 2019–2020 days) |
| Q2 | Feed operations on both books (§6.1)? | **Yes**, behind a `FeedBook` concept | Lets the reference book check the fast book on real data (§8.2) |
| Q3 | Size books with a pre-scan (§7)? | **Yes** | Rule 1 needs sizes up front; real stocks differ too much for one size |
| Q4 | Read in pre-faulted windows (§4)? | **Yes**, 1 GiB default | The file may not fit in WSL2's 15 GB |
| Q5 | Latency samples (§9) | ~~Histogram for all, raw for one stock or 1-in-N~~ **Raw samples of every 8th order message** (changed in step 6, user's choice) | Exact percentiles from 45 M samples in 227 MB, phase 3's Recorder unchanged |
| Q6 | Golden test bytes (§8.3) | From a test encoder, not Nasdaq's file | Small, readable, no republishing question |
| Q7 | Where the file lives (§2) | `data/itch/`, git-ignored; **downloaded now on WSL2** | Work can start now; the C: space it takes is counted before the dual-boot shrink |

---

## 14. Out of scope for phase 4

| Idea | Where it belongs |
|---|---|
| Threads: parser on one core, books on another, over a ring buffer | Phase 5 (README §5 pipeline) |
| Optimising what the replay shows is slow | Phase 6, with before/after numbers (rule 10) |
| `perf` profiles and flame graphs of the replay | Phase 6 (needs bare metal) |
| Live network feed (MoldUDP64, SoupBinTCP) | Not in NanoBook: there is no network (README §1) |
| libFuzzer on the parser | Optional, after phase 4 (README §7.1) |
| Trading on the rebuilt book (signals, strategies) | Not part of an order book. Optional "phase 8" |

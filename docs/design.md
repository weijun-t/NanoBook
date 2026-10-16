# Phase 1 design: reference order book

> **Status:** approved (2026-10-16). Changes from here on are made by an edit plus a note in the commit message.
> Signed off: FIX vocabulary, SMP cancels the resting order, FIX-style total qty on modify,
> `UseCancel`, `NoChange`, strong types, speed-first contract with per-book price band (§12),
> SMP interactions (§6).
> **Revised (2026-10-16), approved scope change:** several symbols behind one `Exchange` that
> assigns order ids from one exchange-wide counter, like Nasdaq (§3.1). This replaces
> sender-chosen ids, `DuplicateId` and the "highest id seen" rule.

The reference book is the slow, obviously correct engine for **one symbol**. An `Exchange` in front
of it (§3.1) owns one book per symbol, numbers every order, and routes each message to its book. Phase 2's fast book must produce the
**same event stream** for the same input, so this note defines that stream exactly. When the two
books disagree, this document decides which one is wrong.

---

## 1. Scope

| In scope | Out of scope (phase 1) |
|---|---|
| Several symbols, one book each, behind one `Exchange` (§3.1). All on one thread. | Threads (e.g. one thread per group of symbols), networking |
| Limit, Market, IOC, FOK, Cancel, Modify | Auctions, stop orders, iceberg orders, pro-rata matching |
| A fixed price band per book (§12) | Moving price bands (real bands follow the current price during the day), circuit breakers / trading halts |
| Price-time (FIFO) priority | A FIX gateway. We borrow FIX **names**, not its wire format. |
| Exchange-wide order ids, assigned by the exchange (§3.1) | Client order references (FIX `ClOrdID`). Calls are synchronous, so the caller already knows which events answer which input. |
| Self-match prevention (SMP) | Publishing latency numbers. That needs the `rdtsc` harness (phase 3) on bare-metal Linux (rule 20). |
| Fixed capacities and an allocation-free **contract** (§12) | |
| Baseline benchmarks of this book (§13) | |

Data structures: `std::map` of price levels per side, `std::list` of orders per level, and a lookup
from `OrderId` to the order. Clarity beats speed **inside this book**.

**Two jobs, two priorities.** The reference book's job is to be obviously correct, so it may allocate.
The **contract** it defines (inputs, events, capacities, id rules) is shared with the phase 2 fast
book, and that book's job is speed. So every rule in this note must allow a fast book that never
allocates after start-up and keeps its hot data in cache. §12 lists the rules that guarantee this.

---

## 2. Types

| Type | Underlying | Notes |
|---|---|---|
| `Price` | `int64_t` | In **ticks** (e.g. 1 tick = $0.01). Never `double` (rule 2). A limit price must be inside the book's price band `[min_price, max_price]` (§12). |
| `Qty` | `int64_t` | Signed on purpose, so a negative input can be detected and rejected instead of wrapping to a huge number. Must be in `[1, max_qty]` (§12). |
| `OrderId` | `uint64_t` | **Assigned by the exchange** from one counter shared by all symbols, starting at 1 (§3.1). `0` means "no id". Never reused within a session. |
| `SymbolId` | `uint16_t` | Which stock. Symbols are numbered `0 … n-1`, so the exchange finds a book with one array index. 65,536 is well above Nasdaq's ~8,000 listings. |
| `OwnerId` | `uint32_t` | Who sent the order (trader, account or algorithm). Used only for SMP. |
| `Side` | `enum class : uint8_t` | `Buy`, `Sell` |
| `OrderType` | `enum class : uint8_t` | `Limit`, `Market`, `IOC`, `FOK` |

**Time priority** comes from a sequence number that the engine increments for each order it
accepts. It never comes from a clock (rule 11: the same input must give the same output).

**Strong types.** `Price`, `Qty`, `OrderId`, `OwnerId` and `SymbolId` are each a one-field struct (for example
`struct Price { int64_t ticks; }`), not a `using` alias. Passing a `Qty` where a `Price` belongs is
then a compile error. They cost nothing at runtime: same size, same registers, same machine code.
Each gets only the operations it needs. `Price` and `Qty` get comparison (`operator<=>`), and `Qty`
gets `+` and `-`. `OrderId` gets comparison too, because "was this id ever issued?" needs `<`.
`OwnerId` and `SymbolId` get `==` only. Each type has a `static_assert` on its size.

---

## 3. Inputs

| Input | Sent by | To | Fields |
|---|---|---|---|
| `OrderRequest` | Trader | Exchange | `symbol`, `owner`, `side`, `type`, `price` (ignored for Market), `qty`. **No id**: the trader doesn't choose one. |
| `NewOrder` | Exchange | Book | Same as `OrderRequest` minus `symbol`, plus the `id` the exchange assigned |
| `Cancel` | Trader | Exchange → book | `symbol`, `id` |
| `Modify` | Trader | Exchange → book | `symbol`, `id`, `new_price`, `new_total_qty` (see §7) |

A modify can only change price and quantity. Side, owner and type stay the same. Only Limit orders
ever rest in the book, so only Limit orders can be modified or cancelled.

`Cancel` and `Modify` carry the `symbol`, as in FIX (an Order Cancel Request, 35=F, requires tag 55
Symbol). The exchange routes them straight to the right book, so the hot path never reads the id
table (§3.1).

### 3.1 The exchange layer

```
Trader ──OrderRequest / Cancel / Modify──► EXCHANGE ──► AAPL book   (ids 1, 3, 5 …)
                                           │ counter     MSFT book   (ids 2 …)
                                           │ id table    TSLA book   (ids 4 …)
```

The `Exchange` owns:
- **One book per symbol**, each built from its own `BookConfig`, stored in an array indexed by `SymbolId`.
- **One order-id counter for the whole exchange.** Every `OrderRequest` that arrives is given the
  next id, **even if it is then rejected**: the id is assigned on receipt, so every `Rejected` event
  has an id the trader can refer to. Each book therefore sees ids that increase but have gaps
  (other symbols' orders, rejected orders). That is exactly what an ITCH feed looks like (phase 4).
- **An id table**: for every id issued this session, which book accepted it, or "none" if it was
  rejected. Fixed size, `max_orders_per_session` entries of 2 bytes each, allocated once at
  start-up (§12).

**Where each check happens.** The exchange checks what only it knows. The book checks the order.

| Input | Exchange checks, in order | Then the book checks (§4, §7) |
|---|---|---|
| `OrderRequest` | `IdsExhausted` (counter reached `max_orders_per_session`; event id is `0`), then assign the id, then `UnknownSymbol` | `InvalidQty`, `InvalidPrice`, `NoLiquidity`, `BookFull` |
| `Cancel` / `Modify` | `UnknownSymbol`, then `UnknownOrder` if the id was never issued (`0`, or above the last id issued) | Not resting → `UnknownOrder` (see below); then Modify steps 3–6 |

**Exact reasons on the cold path.** A book only knows the orders resting in it. If a cancel or modify
names an id that isn't resting there, the book answers `Rejected{UnknownOrder}`. Only on that rare
path does the exchange read the id table to make the reason exact:
- The table says **this symbol** accepted the id → it lived here and has finished →
  `TooLateToCancel`.
- The table says **another symbol**, or **rejected** → it never lived in this book → `UnknownOrder`.

The normal path (order found) never touches the table. A new order writes one table entry. Ids go
up by one, so these writes are sequential and cheap.

The exchange is a template over the book type (`Exchange<Book>`), so the phase 2 fast book plugs in
unchanged. No virtual functions (rule 4).

---

## 4. Events (the receipts)

Every input produces one or more events, appended in order to an output list. The names follow
FIX `ExecType` (tag 150) and `OrdStatus` (tag 39).

| Event | FIX equivalent | Fields |
|---|---|---|
| `New` | ExecType `0` New | `id` |
| `Trade` | ExecType `F` Trade | `aggressor_id`, `resting_id`, `price`, `qty` |
| `Canceled` | ExecType `4` Canceled | `id`, `qty` (amount that will never trade), `reason` |
| `Replaced` | ExecType `5` Replaced | `id`, `new_price`, `new_total_qty` |
| `Rejected` | ExecType `8` Rejected / Order Cancel Reject (35=9) | `id`, `reason` |

The **aggressor** is the incoming order. The **resting** order is the one already in the book.

**Cancel reasons:** `Requested` (trader sent Cancel), `SelfMatch` (§6), `Unfilled` (IOC or Market
remainder), `FokKill` (FOK could not fill completely).

**Reject reasons:**

| Reason | When | FIX equivalent |
|---|---|---|
| `InvalidQty` | qty ≤ 0, or qty > `max_qty` | OrdRejReason 13 Incorrect quantity |
| `InvalidPrice` | Limit, IOC or FOK price outside the price band `[min_price, max_price]` | — |
| `UnknownSymbol` | **Exchange:** the symbol isn't listed | OrdRejReason 1 Unknown symbol |
| `IdsExhausted` | **Exchange:** all `max_orders_per_session` ids have been issued. The event's id is `0`. | OrdRejReason 3 Order exceeds limit |
| `BookFull` | Accepting the order would exceed `max_orders` resting orders | OrdRejReason 3 Order exceeds limit |
| `NoLiquidity` | Market order with nothing (non-self) to trade against | — |
| `UnknownOrder` | Cancel or Modify of an id that never rested in this symbol's book: never issued, rejected, or another symbol's (§3.1). A book on its own reports this for **any** id that isn't resting; the exchange refines it. | CxlRejReason 1 Unknown order |
| `TooLateToCancel` | **Exchange:** Cancel or Modify of an id this book accepted that has since finished (filled or cancelled). **Book:** a modify whose new total ≤ already filled. | CxlRejReason 0 Too late to cancel |
| `UseCancel` | Modify with `new_total_qty == 0` | — |
| `NoChange` | Modify with the same price and same total | — |

A rejected input **changes nothing** in any book. A rejected new order still **uses up** its id,
because ids are assigned on receipt (§3.1). It is recorded as "rejected" in the id table.

**Check order for a new order.** If several checks fail, only the first one is reported, so both
books (and the exchange) must check in the same order: `IdsExhausted`, `UnknownSymbol` (exchange),
then `InvalidQty`, `InvalidPrice` (skipped for Market), `NoLiquidity` (Market only), `BookFull`
(book).

**When exactly is the book full?** `BookFull` is checked **before** matching, and only fires when
the book holds `max_orders` resting orders **and** the new order does not cross (no opposite order
at an acceptable price). The reason: if the order crosses and still has a remainder after matching,
it must have used up every acceptable opposite order, either filled or cancelled by SMP. That frees
at least one slot, so the remainder always fits. IOC, FOK and Market orders never rest, so they are
never `BookFull`. A modify frees its own slot before it re-inserts, so it is never `BookFull` either.

**Ids reaching a book always increase.** The exchange's counter only goes up, so a book can
`assert` (Debug) that each new order's id is above the previous one. The book no longer validates
ids itself.

### Event order for each input (this is the contract)

1. **Validation fails** (exchange or book) → exactly one `Rejected`. Nothing else.
2. **New order accepted** → `New` first, then one event per match step in book order (`Trade`, or
   `Canceled{SelfMatch}` for the trader's own resting order), then at most one closing `Canceled`
   for any remainder (IOC, Market, FOK). A Limit remainder rests silently.
3. **Cancel accepted** → one `Canceled{Requested}` carrying the quantity that was still resting.
4. **Modify accepted** → `Replaced` first. If the order lost priority and its new price crosses,
   matching events follow (rule 2 above).

---

## 5. Matching rules

- **Price first, then time.** An incoming buy matches the lowest ask first. Within one price, the
  oldest order matches first.
- **The trade price is the resting order's price.** A buy at 105 that meets an ask at 100 trades at 100.
- An incoming order keeps matching while it has quantity left **and** the best opposite price is
  acceptable (buy: ask ≤ limit; sell: bid ≥ limit; Market: any price).
- A level whose last order leaves is **removed immediately**.

| Type | Matches? | Remainder | Nothing available |
|---|---|---|---|
| Limit | Yes, up to its price | Rests in the book | `New`, then rests |
| Market | Yes, at any price | `Canceled{Unfilled}` | `Rejected{NoLiquidity}` (FIX OrdStatus 8), no `New` |
| IOC | Yes, up to its price | `Canceled{Unfilled}` | `New`, `Canceled{Unfilled}` (FIX OrdStatus 4) |
| FOK | Only if it can fill **completely** | — | `New`, `Canceled{FokKill}` |

### FOK: look first, then act
1. Without changing anything, add up the **non-self** quantity on the opposite side at acceptable prices.
2. If the total is less than the FOK quantity: `New`, `Canceled{FokKill}`. The book is untouched.
3. Otherwise, match normally. The order is guaranteed to fill completely, so there is never an undo.

---

## 6. Self-match prevention (SMP)

**Rule:** when an incoming order is about to match a resting order with the **same owner**, the
**resting order is cancelled** (`Canceled{SelfMatch}`, carrying its remaining qty). The incoming
order keeps matching against the next resting order.

**Why:** if a strategy sends an order that crosses its own old order, its view of fair value has
changed, so the old order is stale. The new order is the one the strategy wants filled.

Interactions:
- **FOK** ignores own orders when it counts liquidity in step 1. They will be cancelled, not traded.
- **Market** order: if the only liquidity belongs to the same owner, it's `Rejected{NoLiquidity}`
  **before** any matching, so the own resting orders are **not** cancelled. Otherwise the trader would
  lose their old orders and get nothing in return.
- **IOC and Limit**: own orders the incoming order reaches are cancelled as it goes. An IOC that only
  meets its own orders therefore produces `New`, `Canceled{SelfMatch}`…, `Canceled{Unfilled}`.
- **Modify**: a modify whose new price crosses runs matching, and so SMP, exactly like a new order.
- Own orders **beyond** the point where the incoming order is fully filled are not touched.

---

## 7. Modify (cancel/replace)

`new_total_qty` is the new **total** size of the order, **including what has already traded**. This
is FIX's rule for `OrderQty` in a Cancel/Replace request (35=G).

**Why total and not remaining:** a modify can arrive after part of the order traded. Example: order
for 100. The trader wants "50 in total" and sends a modify. Before the engine reads it, 30 trade.

| Meaning of the number | Engine result | Total traded if the rest fills |
|---|---|---|
| Remaining = 50 | 50 resting | 30 + 50 = **80 (overfilled)** |
| **Total = 50** | 50 − 30 = **20 resting** | 30 + 20 = **50 (correct)** |

Rules, checked in this order:
1. **Exchange:** unknown symbol → `Rejected{UnknownSymbol}`. Id never issued → `Rejected{UnknownOrder}`.
2. **Book:** id not resting → `Rejected{UnknownOrder}`, which the exchange refines to
   `TooLateToCancel` if this book had accepted it (§3.1).
3. `new_total_qty == 0` → `Rejected{UseCancel}`. Removal has exactly one path: Cancel.
4. `new_total_qty < 0` or `> max_qty`, or `new_price` outside the price band → `Rejected{InvalidQty}` / `Rejected{InvalidPrice}`.
5. `new_total_qty ≤ filled` → `Rejected{TooLateToCancel}`. A modify never causes an overfill.
6. Same price and same total → `Rejected{NoChange}`. Doing nothing costs nothing, and the trader is
   told that their message had no effect.
7. **Same price and smaller total** → reduce in place and **keep** time priority. Emit `Replaced`.
8. **Otherwise** (bigger total, or a new price) → the order **loses** priority: it moves to the back
   of the queue at the new price, with a new sequence number. Emit `Replaced`, then match if it
   crosses (§5, §6), then rest the remainder.

There is no race **inside** the engine: it is single-threaded and handles one input at a time. The
race is between the sender and the engine, and total-qty semantics make that race safe.

---

## 8. Error handling

- The engine library is built with `-fno-exceptions` (README rule 3). A `throw` will not compile.
- Every error is a `Rejected` event. Internal helpers that can fail return `std::expected`.
- Don't call standard functions that report errors by throwing: no `map::at`, no
  `expected::value()`. Use `find`, and check `has_value()` before using `*`.
- A broken invariant (a bug, not bad input) is checked with `assert` in Debug builds.

---

## 9. Invariants (checked after every test step)

1. The book is not crossed: best bid < best ask, whenever both sides exist.
2. No empty price levels, and no resting order with qty ≤ 0.
3. Each level's total qty equals the sum of its orders' remaining qty.
4. Every id in the lookup is in exactly one level, on the right side, at its own price.
5. Within a level, sequence numbers strictly increase from front to back (FIFO).
6. For every order: filled + remaining = total qty.
7. **Exchange:** every order resting in symbol X's book has id table entry X, and its id is below
   the next id to be issued.

---

## 10. Test list (expected event streams)

Notation: `B` = buy, `S` = sell, `owner/id qty@price`. "Book:" is the state before the input.
Rows 1–32 run against a **book on its own**: the test chooses the ids, and they only have to increase.
Rows E1–E9 run against the **exchange**.

| # | Book | Input | Expected events |
|---|---|---|---|
| 1 | empty | Limit B 1/1 100@10 | `New 1` (rests) |
| 2 | S 2/1 100@10 | Limit B 1/2 100@10 | `New 2`, `Trade 2×1 100@10` |
| 3 | S 2/1 100@10 | Limit B 1/2 40@10 | `New 2`, `Trade 2×1 40@10` (1 keeps 60, still first in queue) |
| 4 | S 2/1 50@10, S 2/2 50@11 | Limit B 1/3 80@11 | `New 3`, `Trade 3×1 50@10`, `Trade 3×2 30@11` |
| 5 | S 2/1 100@10 | Limit B 1/2 50@12 | `New 2`, `Trade 2×1 50@10` (trades at resting price) |
| 6 | S 2/1 100@10 | Cancel 1 | `Canceled 1 qty 100 Requested` (level 10 removed) |
| 7 | empty | Cancel 9 | `Rejected 9 UnknownOrder` (book: not resting) |
| 8 | (1 fully filled earlier) | Cancel 1 | `Rejected 1 UnknownOrder` (book on its own; the exchange refines this, E5) |
| 9 | empty | Market B 1/1 100 | `Rejected 1 NoLiquidity` |
| 10 | S 2/1 30@10 | Market B 1/2 100 | `New 2`, `Trade 2×1 30@10`, `Canceled 2 qty 70 Unfilled` |
| 11 | empty | IOC B 1/1 100@10 | `New 1`, `Canceled 1 qty 100 Unfilled` |
| 12 | S 2/1 60@10 | FOK B 1/2 100@10 | `New 2`, `Canceled 2 qty 100 FokKill` (book unchanged) |
| 13 | S 2/1 60@10, S 2/2 40@10 | FOK B 1/3 100@10 | `New 3`, `Trade 3×1 60@10`, `Trade 3×2 40@10` |
| 14 | S 1/1 50@10, S 2/2 50@10 | Limit B 1/3 50@10 | `New 3`, `Canceled 1 qty 50 SelfMatch`, `Trade 3×2 50@10` |
| 15 | S 1/1 50@10 | Market B 1/2 50 | `Rejected 2 NoLiquidity` (own order untouched) |
| 16 | S 1/1 50@10, S 2/2 50@10 | FOK B 1/3 100@10 | `New 3`, `Canceled 3 qty 100 FokKill` (only 50 non-self) |
| 17 | B 1/1 100@10, B 2/2 100@10 | Modify 1 → 10, total 60 | `Replaced 1` (1 keeps first place) |
| 18 | B 1/1 100@10, B 2/2 100@10 | Modify 1 → 10, total 150 | `Replaced 1` (1 now behind 2) |
| 19 | B 1/1 100@10 (30 filled) | Modify 1 → 10, total 50 | `Replaced 1` (20 resting) |
| 20 | B 1/1 100@10 (30 filled) | Modify 1 → 10, total 30 | `Rejected 1 TooLateToCancel` |
| 21 | B 1/1 100@10 | Modify 1 → 10, total 0 | `Rejected 1 UseCancel` |
| 22 | B 1/1 100@10, S 2/2 100@12 | Modify 1 → 12, total 100 | `Replaced 1`, `Trade 1×2 100@12` |
| 23 | — | *(removed: senders no longer choose ids, so `DuplicateId` can't happen)* | — |
| 24 | any | Limit B 1/5 0@10 / -5@10 | `Rejected 5 InvalidQty` |
| 25 | B 1/1 100@10 | Modify 1 → 10, total 100 | `Rejected 1 NoChange` |
| 26 | — | *(removed: see 23)* | — |
| 27 | — | *(moved to the exchange: E4)* | — |
| 28 | `max_orders` resting | Limit B 1/n 10@1 (would rest) | `Rejected n BookFull` |
| 29 | `max_orders` resting, S 2/1 10@10 | Limit B 1/n 10@10 (fills fully) | `New n`, `Trade n×1 10@10` (never rests, so not full) |
| 30 | any | Limit B 1/n 10@(max_price + 1) | `Rejected n InvalidPrice` |
| 31 | any | Limit B 1/n 10@(min_price − 1) | `Rejected n InvalidPrice` |
| 32 | any | Limit B 1/n (max_qty + 1)@10 | `Rejected n InvalidQty` |

Exchange tests. Symbols `A` and `M`. Ids are what the exchange assigns.

| # | State before | Input | Expected events |
|---|---|---|---|
| E1 | empty | Buy A, Buy M, Buy A | ids `1`, `2`, `3`: one counter for all symbols. Book A holds 1 and 3, book M holds 2. |
| E2 | empty | Buy A with qty 0, then Buy A | `Rejected 1 InvalidQty`, then `New 2`: a rejected order uses up its id |
| E3 | empty | Buy on an unlisted symbol | `Rejected 1 UnknownSymbol` (id 1 used up) |
| E4 | ids 1–7 issued | Cancel A #8, and Cancel A #0 | `Rejected 8 UnknownOrder`, `Rejected 0 UnknownOrder`: never issued |
| E5 | #1 accepted in A, then fully filled | Cancel A #1 | `Rejected 1 TooLateToCancel`: it lived in A and finished |
| E6 | #1 resting in A | Cancel M #1 | `Rejected 1 UnknownOrder`: wrong symbol; #1 never lived in M. A is untouched. |
| E7 | #1 rejected (bad qty) | Cancel A #1 | `Rejected 1 UnknownOrder`: it never entered a book |
| E8 | all `max_orders_per_session` ids issued | Buy A | `Rejected 0 IdsExhausted`: no id left to give |
| E9 | #1 accepted in A, then fully filled | Modify A #1 → total 50 | `Rejected 1 TooLateToCancel`: the same refinement as E5, for Modify |

---

## 11. Hook for phase 2

- A C++20 **concept** (`OrderBook`) lists the operations both books must provide: submit an input,
  read the events, query best bid/ask and depth. No virtual functions (rule 4).
- The unit tests are written once as GoogleTest `TYPED_TEST`s and run against every type that
  satisfies the concept. In phase 2 the fast book is added to the type list, and every test above
  runs on it unchanged.
- The differential fuzzer feeds both books the same random inputs and compares their event lists
  element by element.
- The exchange is `Exchange<Book>`, so the exchange tests (E1–E9) can also run over the fast book.

---

## 12. Speed and memory rules for the contract

The reference book follows these rules **on the outside**, so the fast book can follow them on the
inside without changing any behaviour.

| Rule | What it means | Why it's fast |
|---|---|---|
| **Per-book config, set at construction** | `BookConfig { min_price, max_price, max_orders, max_qty }`. Both books take the same config. Each symbol gets its own (inside `ExchangeConfig`) (a $5 stock and a $700,000 stock need very different bands). | Everything the fast book needs can be sized **once at start-up**. |
| **Fixed order capacity `max_orders`** | A full book gives `Rejected{BookFull}` (rule 8). Choose it generously: room for 1M orders is tens of MB. | An order pool of `max_orders` slots, allocated once. Nothing allocates while trading (rule 1). Real engines are sized in advance in the same way. |
| **Maximum order size `max_qty`** | An order or modify whose (total) qty is above it is `InvalidQty`. A config is only valid if `max_orders × max_qty` fits in 64 bits. | **Correctness, not speed**: every sum the book computes (a level's qty, the look-first count) adds at most `max_orders` orders of at most `max_qty` each, so signed overflow (undefined behaviour) becomes impossible. It costs one comparison per order. Real exchanges have the same limit as a fat-finger check. |
| **Price band `[min_price, max_price]`** | Prices outside it are `InvalidPrice`. `min_price ≥ 1`. | **Mainly a real exchange rule**: exchanges reject prices outside a band (e.g. US Limit Up-Limit Down, price collars), much like fat-finger checks. The fast book **must work correctly for any band**, including very wide ones. Whether to exploit a narrow band with a tick-indexed array, or to use a sorted vector of levels that needs no band, is a phase 2 **benchmark** decision (README §5). |
| **Exchange-assigned ids and a fixed id table** | `ExchangeConfig { max_orders_per_session, one BookConfig per symbol }`. The table has one 2-byte entry per id, allocated once (100M orders/day ≈ 200 MB). When every id is used, new orders get `IdsExhausted`. | **Hot path:** a new order writes one entry. Ids go up by one, so the writes are sequential and the CPU prefetches them. Cancel and Modify carry the symbol, so they **never read** the table unless the order isn't found (the cold path). The real cost (cache space used by the table writes) gets measured in phase 2 and 3, not guessed (rule 10). |
| **Events go into a caller-provided buffer** | The engine appends to a fixed-capacity array that the caller owns. Worst case per input: `max_orders + 2` events (a sweep that touches every resting order, plus `New` and a closing `Canceled`). | No `vector` growth per input. The buffer is reused, so it stays in cache. |
| **Events are small, fixed-size structs** | Plain integers and enums only: no strings, no pointers, no `std::variant`. Each event's `sizeof` is `static_assert`ed. | Copying an event is a few register moves. A trade (four 8-byte fields plus a tag) is about 40 bytes, so events pack tightly into 64-byte cache lines. |
| **No clock, no I/O, no exceptions** | Sequence numbers instead of time. No logging inside the engine (rules 6, 7). `-fno-exceptions`. | No syscalls and no surprise slow paths. |

Where the speed actually comes from (phase 2, measured there, not here):
- **Order pool** of `max_orders` slots addressed by `uint32_t` index, not pointers. Indices are half
  the size of pointers, so more orders fit in each cache line.
- **Intrusive FIFO lists**: each order stores the `prev` and `next` indices of its neighbours, so
  cancel is O(1) and allocates nothing.
- **Hot fields first**: price, qty, side and the list links sit together in the first 64 bytes of an
  order. Cold fields (owner, original total) come after them.
- The README's §4 table of `std::map` and `std::list` costs is the "before" picture that phase 2 must
  beat with numbers.

---

## 13. Benchmarks in phase 1 (baseline only)

Phase 1 records **baseline** numbers for the reference book. They are not a target. They're the
"before" column that phase 2 must improve, measured the same way so the comparison is fair.

| Benchmark (Google Benchmark, `nanobook_microbench`) | Reports |
|---|---|
| Add a limit order that rests (no match) | ns/op (mean), ops/sec |
| Cancel a resting order | ns/op, ops/sec |
| Aggressive order that matches one resting order | ns/op, ops/sec |
| Sweep through N levels (N = 1, 10, 100) | ns/op, and how it grows with N |
| Mixed stream: seeded random 60% add, 30% cancel, 10% match | ops/sec |
| **Allocations per operation** (counting `operator new` hook) | Number (expected > 0 here; phase 2 must reach 0) |

Rules for these numbers:
- Release preset, same flags for both books. Each result is recorded with CPU, OS/kernel, compiler,
  flags and the WSL2-or-native question (rule 19), in `docs/perf-log.md`.
- Google Benchmark gives **averages**, not tail latency (rule 17). The p50/p99/p99.9 numbers the
  README promises come from the `rdtsc` harness in phase 3.
- Numbers measured on WSL2 or CI are for **comparing** books on the same machine only. They are
  never published (rule 20).
- The README's target for the **fast** book is tens to low hundreds of ns at p50 for add, cancel and
  execute. A `std::map` book is expected to be slower, typically because every new level and order
  allocates and lives far apart in memory. The baseline shows by how much.

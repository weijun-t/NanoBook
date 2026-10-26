# Depth design: one stock's L2 ladder and L3 queues, rebuilt by its own replay

> **Status:** draft, for review (2026-10-26). Option A of the three the user was offered: a
> separate program replays the day itself, with NanoBook's own book, for one chosen stock. No
> engine file, no pipeline file and no published number changes.

The L1 viewer (`docs/viewer-design.md`) shows every stock's best bid and ask. This note designs
the next view: **one stock's whole book**, by price (L2) and order by order (L3), live, with the
order flow that changes it.

---

## 0. Background in plain words

### 0.1 Why the L1 viewer can't simply show more

The L1 viewer works like a departures board: it reads each stock's latest line whenever it
likes, and skipping 1,699 updates between two redraws does no harm. A book can't be copied that
way. To keep a correct copy you need **every** message, in order: miss one cancel and an order
that's gone stays on screen for ever. So a deeper view either slows the pipeline down (it must
wait for the viewer) or gets the book wrong (it drops messages). Option A avoids both by not
taking anything from the pipeline at all.

### 0.2 How a real firm does it

Nasdaq doesn't send anyone its book. It sends the **feed**, every order message, and each firm
rebuilds its own copy (phase 4). Two firms watching AAPL each run their own copy. This tool is a
second firm: it reads the same day's file and rebuilds the chosen stock's book with the same
`FastBook` code the engine uses, in its own process.

### 0.3 What the book can already tell us (no engine change)

`FastBook` has read-only queries, written for tests and plots:

| Query | Gives | Level |
|---|---|---|
| `best_bid()`, `best_ask()` | The two best prices | L1 |
| `depth(side, n)` | The best `n` levels of a side: (price, total shares) | L2 |
| `level_orders(side, price)` | The order IDs resting at one price, **front of the queue first** | L3 |
| `leaves(id)` | An order's remaining shares | L3 |

They allocate (they return `std::vector`), which is why the engine's hot path never calls them.
Here that's fine: this is another process, ten redraws a second, nowhere near the pipeline. So
**no file under `include/` or `src/` changes.**

---

## 1. Goal and exit criteria

1. `nanobook_depth FILE SYMBOL` shows, live, one stock's book:
   - **L2:** the best 10 price levels on each side: price, total shares, number of orders.
   - **L3:** the queue at one chosen level: each order in time priority, its shares, when it
     arrived, and the shares ahead of it.
   - **Tape:** the stock's latest order messages (add, execute, cancel, delete, replace).
2. **Pause and step:** freeze the replay, then apply one message at a time and watch what it
   does to the book. **Follow** one order and see it move up its queue until it fills or leaves.
3. **Correct:** at the end of the day, the chosen stock's book equals the one the full replay
   builds (every stock), for AAPL, SPY and QQQ at least. A reference book runs alongside (§4.3)
   and the screen says if they ever disagree.
4. **Nothing else changes:** `git diff v1.0.0 -- include src` is empty; the pipeline, the L1
   viewer and every published number are untouched. CI green, as always.
5. Built only with `-DNANOBOOK_BUILD_VIEWER=ON`, like the L1 viewer.

---

## 2. Shape

```
 ITCH file (mmap, read-only)
     │  every message, in order
     v
 parser ──> StockFilter ──> BookBuilder<FastBook>  (one stock's book)      ┐
              │  drops order messages      + ReferenceBook (the check)      │  DepthReplay
              │  of every other stock      + per-order notes, tape          ┘  (support/)
              v
 nanobook_depth (one thread: replay up to "now", then draw; FTXUI)
```

New pieces, all outside the engine:
- `support/depth_replay.hpp`: **DepthReplay**, the replay of one stock: reads messages from the
  file one at a time, applies those of the chosen stock, keeps notes and the tape (§3, §4). No UI
  code, so it's tested in the main suite.
- `tools/depth_render.hpp`: the screen layout, tested off-screen like `view_render.hpp`.
- `tools/nanobook_depth.cpp`: the loop, keys and pacing.

---

## 3. The replay

### 3.1 One stock, filtered by locate

Every ITCH message carries the stock's **locate** (its number for the day). `StockFilter` is a
parser handler in front of `BookBuilder`: it passes on Stock Directory (R), trading state (H)
and system events (S) for everyone, and order messages (A, F, E, C, X, D, U) only when their
locate is the chosen stock's. `BookBuilder` is reused as it is; only one of its books is ever
touched.

### 3.2 Sizing the book

`FastBook` is sized before the day starts (rule 1): room for the stock's peak resting orders and
its price range. Start-up runs the same **counting pass** the pipeline runs (`DayCounter`), then
`size_book()` for the chosen stock, and every other locate gets the smallest valid book. About
the pipeline's start-up time; it's the price of using the real engine instead of an unbounded
map.

### 3.3 One thread, no locks

Ten times a second, the UI's timer asks the replay to **advance to now**: apply every message
whose ITCH time is at or before `from + elapsed × speed`, then draw. The book is only ever
touched by that one thread, so there's nothing to lock and nothing to tear. At 60× a tick is 6 s
of the day: about 130,000 messages to parse, of which a few hundred to a few thousand are the
chosen stock's. Parsing is a few ns a message, so a tick costs well under 10 ms.

- **Pause** stops the clock. **Step** applies the stock's next message and nothing else.
  **Speed** up or down re-anchors the clock at the current ITCH time, so nothing jumps.
- Before `--from`, the replay runs unpaced (as `nanobook_pipeline --speed` does).

### 3.4 Running it next to the L1 viewer

Both tools take `--speed` and `--from`, so started together they show about the same moment.
They aren't locked together: each paces from its own start, and their start-up times differ by
a few seconds. That's fine for watching; neither depends on the other.

---

## 4. What the screen shows

### 4.1 Layout (about 110 × 40)

```
╭ NanoBook depth  AAPL ───────────────────────────── ITCH 09:37:05.021   60.0x   running ─╮
│ bid 162.0700 × 100    ask 162.1100 × 100    spread 0.0400 (2.47 bps)    1,234 resting   │
│ last trade 162.0900 × 100 at 09:37:05.019   messages 134,724   reference: matches       │
├──────────────── BIDS ───────────────────┬──────────────── ASKS ─────────────────────────┤
│  ORDERS     SHARES        PRICE         │        PRICE       SHARES   ORDERS            │
│ >     1        100     162.0700         │     162.1100          100        1            │
│       3        500     162.0600         │     162.1200          300        1            │
│      ...  (10 levels a side)            │      ...                                      │
├──────────────── QUEUE  bid 162.0700 (1 order, 100 shares) ──────────────────────────────┤
│   #   ORDER ID       SHARES   AHEAD   SINCE           ATTRIBUTION                       │
│   1   402,991,227       100       0   09:37:04.871                                      │
├──────────────── TAPE  AAPL, newest first ───────────────────────────────────────────────┤
│ 09:37:05.019   EXEC     402,990,880   100 @ 162.0900   (the best ask's front order)     │
│ 09:37:05.011   ADD      402,991,227   B 100 @ 162.0700                                  │
│ 09:37:05.002   CANCEL   402,990,512   300 @ 162.1200                                    │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ ↑↓ level  tab side  space pause  n step  f follow  +/- speed  q quit                    │
╰─────────────────────────────────────────────────────────────────────────────────────────╯
```

- **L2 ladder:** 10 levels a side, best at the top, ORDERS from `level_orders().size()`.
  `>` marks the selected level, whose queue is shown below.
- **Queue (L3):** the selected level's orders, front first. AHEAD is the shares in front of this
  order: what must trade or cancel before it fills (price-time priority, made visible). SINCE
  and ATTRIBUTION (an F message's market maker, if any) come from DepthReplay's own notes on each
  order, taken when it was added: the book doesn't keep them, and doesn't need to.
- **Tape:** the last 12 messages of this stock. An execution (E) carries no price; the price is
  the order's, from the notes.
- **Followed order:** highlighted wherever it is; the header then reads "following 402,991,227:
  2nd in line, 300 shares ahead", and "filled" or "cancelled" when it leaves.

Prices are integers printed with integer division (`162.0700`), as in the L1 viewer; only bps is
floating point (display only, rule 2).

### 4.2 Keys

| Key | Does |
|---|---|
| ↑ / ↓ | Choose a level; its queue is shown |
| Tab | Switch between the bid and ask side |
| Space | Pause / resume |
| `n` | While paused: apply the stock's next message |
| `f` | Follow the order at the top of the shown queue (again: stop following) |
| `+` / `-` | Double / halve the speed |
| `q` | Quit |

### 4.3 The live check

A `ReferenceBook` (phase 1's slow, obviously correct book) gets the same messages. After every
message of the stock, the replay compares the cheap things: both books' result, best bid, best
ask, and the shares at the price the message touched (`level_qty`, no allocation). Once a tick it
also compares the best 10 levels and the shown queue. Any difference turns "reference: matches"
red and pauses the replay right after the message that caused it. For one stock it costs little,
and it shows the differential testing of phases 2 and 4 working live.

---

## 5. Testing

- **DepthReplay, synthetic** (ITCH bytes from `support/itch_encoder.hpp`, main suite):
  messages of two stocks, only the chosen one's applied; levels and queues as expected after
  each step; AHEAD and SINCE right after adds, partial executions, cancels and a replace (which
  loses its place in the queue); the tape newest first and capped; pause, step and re-anchoring
  the clock with a fake clock; following an order to its fill.
- **The live check fires:** a planted wrong message order makes the two books differ, and the
  replay stops at that message.
- **The real day** (skipped if the file isn't there, like the other ITCH tests): for AAPL, SPY
  and QQQ, the end-of-day book (`resting_ids()` and every level) equals the full replay's book for
  that locate, and the reference never disagreed.
- **Layout**, off-screen: a frame drawn from a small synthetic book, read back as text; the
  queue and tape fit 110 columns.
- **Planted bugs**, as in every phase: e.g. the filter passing another stock's cancel, AHEAD
  counting the order itself, the tape dropping replaces. Each must fail a test.
- Engine untouched: `git diff v1.0.0 -- include src` empty.

---

## 6. Plan, one commit per step

1. This note, approved.
2. `support/depth_replay.hpp` (filter, notes, tape, clock), with its tests.
3. The live check (§4.3) and the real-day end-of-day comparison.
4. `tools/depth_render.hpp` and layout tests.
5. `tools/nanobook_depth.cpp`: loop, keys, pacing; CI builds it in the viewer job; tried by hand
   on the real day.
6. README "Watch one stock" with a real frame; a short report (`docs/depth-report.md` + PDF),
   including how to run it.

---

## 7. Decisions (for review)

| # | Question | Proposal |
|---|---|---|
| D1 | Which book drives the screen? | **`FastBook`**, the engine's own, with `ReferenceBook` alongside as the live check (§4.3). Showing the real engine is the point. |
| D2 | Sizing | **The counting pass** (§3.2), as the pipeline does: a few seconds more at start-up, no guessing. |
| D3 | Threads | **One** (§3.3): replay and draw on the UI thread, no locks. A replay thread would only matter if a tick took longer than 100 ms. |
| D4 | One stock or several? | **One per run**, on the command line. Switching stock means replaying it from 04:00; a second terminal does that already. |
| D5 | Levels shown | **10 a side**, `--levels N` to change it. |
| D6 | Branch | **Merge `ui` into `main` first** (the viewer is finished), then a new branch `depth` from `main`. |
| D7 | Name | **`nanobook_depth`**, built with the viewer's option and FTXUI. |

---

## 8. Out of scope for this version

| Idea | Why not now |
|---|---|
| Watching the pipeline's own books | Needs the engine to publish depth (option B) or every message (option C): engine-side cost, measured on bare metal |
| Several stocks at once, or switching inside the tool | D4; a second terminal does it |
| Charts (price over time, queue length over time) | Needs history; could be recorded from DepthReplay later |
| Market-by-price feeds other than ITCH | Only ITCH 5.0 is parsed |

/*
 * reference_book.hpp: the reference order book: slow on purpose, obviously correct.
 *
 * What it does: Holds the resting orders for one stock and matches incoming orders by price-time
 *   priority: best price first, oldest first within a price, trading at the resting order's
 *   price. Supports Limit, Market, IOC and FOK orders, Cancel and Modify, with self-match
 *   prevention. As a copy of another exchange's book (phase 4) it applies feed inputs instead:
 *   add without matching, reduce, remove, replace.
 *
 * Why it exists: It is the answer key. The phase 2 fast book is checked against it on millions of
 *   random inputs, so it is written for clarity, not speed, and may allocate freely.
 *
 * How it works: Matching walks the best opposite level's queue from the front, trading until the
 *   incoming order is filled or the price no longer crosses. FOK first counts what it could fill
 *   without changing anything, so it never has to undo a trade. Modify keeps queue priority only
 *   when it shrinks the order at the same price.
 *
 * Data structures: Bids and asks are std::map<Price, std::list<Order>>: a red-black tree of price
 *   levels (O(log n) lookup), each level a FIFO linked list. Orders by id: std::map<OrderId,
 *   Locator>, where the Locator holds a std::list iterator, so a cancel unlinks its order directly
 *   without searching the queue.
 *
 * Trade-offs: Every order and every level is a separate heap allocation (about 2 per added order),
 *   scattered in memory, so lookups miss the CPU cache. That is exactly what the fast book removes.
 *   A book only knows its resting orders, so any other id is UnknownOrder here, and the exchange
 *   refines it.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook {

// Design §1-§9. Trusts the ids it is given: the Exchange assigns them, always increasing
// (design §3.1).
class ReferenceBook {
public:
    explicit ReferenceBook(BookConfig config);

    // Each call clears `out`, then writes the events for this one input, in the order fixed by
    // design §4. `out` must hold at least config.max_events_per_input() events.
    void handle(const NewOrder &order, EventBuffer &out);
    void handle(const Cancel &cancel, EventBuffer &out);
    void handle(const Modify &modify, EventBuffer &out);

    // Feed inputs (ITCH design §6.1): copy another exchange's book. Never match, emit no events;
    // anything but Ok leaves the book unchanged.
    [[nodiscard]] FeedResult apply(const FeedAdd &add);
    [[nodiscard]] FeedResult apply(const FeedReduce &reduce);
    [[nodiscard]] FeedResult apply(const FeedRemove &remove);
    [[nodiscard]] FeedResult apply(const FeedReplace &replace);

    // Read-only queries, for tests and invariant checks.
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Qty level_qty(Side side, Price price) const;                     // 0 if no level
    [[nodiscard]] std::vector<OrderId> level_orders(Side side, Price price) const; // front first
    // Up to max_levels levels, best first: (price, total qty).
    [[nodiscard]] std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t max_levels) const;
    [[nodiscard]] std::optional<Qty> leaves(OrderId id) const; // remaining qty, if resting
    [[nodiscard]] std::size_t order_count() const;             // resting orders
    [[nodiscard]] std::vector<OrderId> resting_ids() const;    // ascending; for invariant checks
    [[nodiscard]] bool invariants_hold() const;                // design §9
    [[nodiscard]] bool structure_holds() const;                // design §9 minus "not crossed"

private:
    struct Order {
        OrderId id{};
        OwnerId owner{};
        Side side{Side::Buy};
        Price price{};
        Qty total{};          // full size, including what has already traded
        Qty filled{};         // traded so far
        std::uint64_t seq{0}; // time priority: lower = arrived earlier (design §2)

        [[nodiscard]] Qty leaves() const {
            return total - filled;
        }
    };

    using Queue = std::list<Order>;                      // FIFO: front = oldest
    using Bids = std::map<Price, Queue, std::greater<>>; // begin() = highest bid = best
    using Asks = std::map<Price, Queue, std::less<>>;    // begin() = lowest ask = best

    // Where a resting order lives, so Cancel and Modify can find it without searching.
    // std::list iterators stay valid while other orders are added or removed.
    struct Locator {
        Side side{Side::Buy};
        Price price{};
        Queue::iterator it;
    };

    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder &order) const;
    [[nodiscard]] bool crosses_book(Side side, Price limit) const;
    [[nodiscard]] std::optional<RejectReason> validate(const Modify &modify,
                                                       const Order &current) const;

    using Lookup = std::map<OrderId, Locator>;

    // Side dispatch: each picks bids_ or asks_ and calls the matching template below.
    Qty match(OrderId aggressor, OwnerId owner, Side side, Price limit, Qty qty, EventBuffer &out);
    void rest(const Order &order);
    void unlink(Lookup::iterator found); // removes a resting order from its level and the lookup

    template <typename Levels>
    Qty match_against(Levels &opposite, OrderId aggressor, OwnerId owner, Side side, Price limit,
                      Qty remaining, EventBuffer &out);
    template <typename Levels> void insert(Levels &levels, const Order &order);
    template <typename Levels> void remove(Levels &levels, const Locator &where);

    BookConfig config_;
    Bids bids_;
    Asks asks_;
    Lookup orders_;     // resting orders only
    OrderId last_id_{}; // id of the previous NewOrder; ids must increase
    std::uint64_t next_seq_{0};
};

// Proves at compile time that the reference book meets the contract (design §11).
static_assert(OrderBook<ReferenceBook>);
static_assert(FeedBook<ReferenceBook>);

} // namespace nanobook

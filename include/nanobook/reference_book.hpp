#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <vector>

#include "nanobook/event_buffer.hpp"
#include "nanobook/messages.hpp"
#include "nanobook/types.hpp"

namespace nanobook {

// Phase 1 reference book (design §1). Slow on purpose and written to be obviously correct:
// a std::map of price levels per side, a std::list (FIFO queue) of orders per level, and a
// std::map from order id to where the order sits. It allocates freely. The phase 2 fast book
// must produce exactly the same events for the same inputs.
//
// Supported so far: Limit orders and Cancel. Market, IOC, FOK, self-match prevention and
// Modify come in the next commits.
class ReferenceBook {
public:
    explicit ReferenceBook(BookConfig config);

    // Each call clears `out`, then writes the events for this one input, in the order fixed by
    // design §4. `out` must hold at least config.max_events_per_input() events.
    void handle(const NewOrder &order, EventBuffer &out);
    void handle(const Cancel &cancel, EventBuffer &out);

    // Read-only queries, for tests and invariant checks.
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Qty level_qty(Side side, Price price) const;                     // 0 if no level
    [[nodiscard]] std::vector<OrderId> level_orders(Side side, Price price) const; // front first
    [[nodiscard]] std::optional<Qty> leaves(OrderId id) const; // remaining qty, if resting
    [[nodiscard]] std::size_t order_count() const;             // resting orders
    [[nodiscard]] bool invariants_hold() const;                // design §9

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

    // Where a resting order lives, so Cancel can find it without searching.
    // std::list iterators stay valid while other orders are added or removed.
    struct Locator {
        Side side{Side::Buy};
        Price price{};
        Queue::iterator it;
    };

    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder &order) const;
    [[nodiscard]] bool crosses_book(Side side, Price limit) const;
    [[nodiscard]] RejectReason unknown_or_too_late(OrderId id) const;

    template <typename Levels>
    Qty match_against(Levels &opposite, OrderId aggressor, Side side, Price limit, Qty remaining,
                      EventBuffer &out);
    template <typename Levels> void insert(Levels &levels, const Order &order);
    template <typename Levels> void remove(Levels &levels, const Locator &where);

    BookConfig config_;
    Bids bids_;
    Asks asks_;
    std::map<OrderId, Locator> orders_; // resting orders only
    std::optional<OrderId> highest_id_; // highest accepted id; empty until the first order
    std::uint64_t next_seq_{0};
};

} // namespace nanobook

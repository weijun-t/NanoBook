#include "nanobook/reference_book.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iterator>

namespace nanobook {

namespace {

// Can an incoming order on `side` with limit `limit` trade against a resting order at `level`?
// A buy accepts asks at or below its limit; a sell accepts bids at or above its limit.
bool acceptable(Side side, Price limit, Price level) {
    return side == Side::Buy ? level <= limit : level >= limit;
}

} // namespace

ReferenceBook::ReferenceBook(BookConfig config) : config_(config) {
    assert(config_.is_valid());
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

void ReferenceBook::handle(const NewOrder &order, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    if (order.type != OrderType::Limit) {
        std::abort(); // Market, IOC and FOK are implemented in the next commit
    }

    if (const std::optional<RejectReason> reason = validate(order)) {
        out.push(Event::make_rejected(order.id, *reason));
        return; // a rejected input changes nothing, not even highest_id_ (design §4)
    }

    highest_id_ = order.id;
    out.push(Event::make_new(order.id));

    // Match against the opposite side, then rest whatever is left (Limit only).
    const Qty remaining =
        order.side == Side::Buy
            ? match_against(asks_, order.id, order.side, order.price, order.qty, out)
            : match_against(bids_, order.id, order.side, order.price, order.qty, out);

    if (remaining > Qty{0}) {
        const Order resting{.id = order.id,
                            .owner = order.owner,
                            .side = order.side,
                            .price = order.price,
                            .total = order.qty,
                            .filled = order.qty - remaining,
                            .seq = next_seq_++};
        if (order.side == Side::Buy) {
            insert(bids_, resting);
        } else {
            insert(asks_, resting);
        }
    }
}

void ReferenceBook::handle(const Cancel &cancel, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    const auto found = orders_.find(cancel.id);
    if (found == orders_.end()) {
        out.push(Event::make_rejected(cancel.id, unknown_or_too_late(cancel.id)));
        return;
    }

    const Locator where = found->second;
    const Qty remaining = where.it->leaves();
    orders_.erase(found);
    if (where.side == Side::Buy) {
        remove(bids_, where);
    } else {
        remove(asks_, where);
    }
    out.push(Event::make_canceled(cancel.id, remaining, CancelReason::Requested));
}

// ---------------------------------------------------------------------------
// Checks (design §4: the order of these checks is part of the contract)
// ---------------------------------------------------------------------------

std::optional<RejectReason> ReferenceBook::validate(const NewOrder &order) const {
    if (highest_id_ && order.id <= *highest_id_) {
        return RejectReason::DuplicateId;
    }
    if (order.qty <= Qty{0}) {
        return RejectReason::InvalidQty;
    }
    if (!config_.in_band(order.price)) {
        return RejectReason::InvalidPrice;
    }
    // Full only matters if the order would rest without matching anything (design §4).
    if (orders_.size() >= config_.max_orders && !crosses_book(order.side, order.price)) {
        return RejectReason::BookFull;
    }
    return std::nullopt;
}

bool ReferenceBook::crosses_book(Side side, Price limit) const {
    if (side == Side::Buy) {
        return !asks_.empty() && acceptable(side, limit, asks_.begin()->first);
    }
    return !bids_.empty() && acceptable(side, limit, bids_.begin()->first);
}

// Ids are strictly increasing (design §12), so one number tells "never sent" from "finished".
RejectReason ReferenceBook::unknown_or_too_late(OrderId id) const {
    if (!highest_id_ || id > *highest_id_) {
        return RejectReason::UnknownOrder;
    }
    return RejectReason::TooLateToCancel;
}

// ---------------------------------------------------------------------------
// Matching (design §5): best price first, oldest first within a price, trade at resting price
// ---------------------------------------------------------------------------

template <typename Levels>
Qty ReferenceBook::match_against(Levels &opposite, OrderId aggressor, Side side, Price limit,
                                 Qty remaining, EventBuffer &out) {
    while (remaining > Qty{0} && !opposite.empty()) {
        const auto level = opposite.begin(); // best opposite price
        if (!acceptable(side, limit, level->first)) {
            break;
        }

        Queue &queue = level->second;
        // Levels are erased the moment they empty (below, and in remove()). If one is ever left
        // behind, this loop would pick it again forever, so stop loudly instead (Debug only).
        assert(!queue.empty() && "empty price level left in the book");
        while (remaining > Qty{0} && !queue.empty()) {
            Order &resting = queue.front(); // oldest order at this price
            const Qty traded = std::min(remaining, resting.leaves());
            out.push(Event::make_trade(aggressor, resting.id, resting.price, traded));
            resting.filled += traded;
            remaining -= traded;

            if (resting.leaves() == Qty{0}) {
                orders_.erase(resting.id);
                queue.pop_front();
            }
        }

        if (queue.empty()) {
            opposite.erase(level); // no empty levels (design §9)
        }
    }
    return remaining;
}

template <typename Levels> void ReferenceBook::insert(Levels &levels, const Order &order) {
    Queue &queue = levels[order.price]; // creates the level if it doesn't exist yet
    queue.push_back(order);             // back of the queue = lowest time priority
    orders_.emplace(order.id, Locator{order.side, order.price, std::prev(queue.end())});
}

template <typename Levels> void ReferenceBook::remove(Levels &levels, const Locator &where) {
    const auto level = levels.find(where.price);
    assert(level != levels.end());
    level->second.erase(where.it);
    if (level->second.empty()) {
        levels.erase(level);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> ReferenceBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> ReferenceBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Qty ReferenceBook::level_qty(Side side, Price price) const {
    const auto sum = [](const Queue &queue) {
        Qty total{};
        for (const Order &order : queue) {
            total += order.leaves();
        }
        return total;
    };
    if (side == Side::Buy) {
        const auto level = bids_.find(price);
        return level == bids_.end() ? Qty{} : sum(level->second);
    }
    const auto level = asks_.find(price);
    return level == asks_.end() ? Qty{} : sum(level->second);
}

std::vector<OrderId> ReferenceBook::level_orders(Side side, Price price) const {
    const auto ids = [](const Queue &queue) {
        std::vector<OrderId> result;
        for (const Order &order : queue) {
            result.push_back(order.id);
        }
        return result;
    };
    if (side == Side::Buy) {
        const auto level = bids_.find(price);
        return level == bids_.end() ? std::vector<OrderId>{} : ids(level->second);
    }
    const auto level = asks_.find(price);
    return level == asks_.end() ? std::vector<OrderId>{} : ids(level->second);
}

std::optional<Qty> ReferenceBook::leaves(OrderId id) const {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return std::nullopt;
    }
    return found->second.it->leaves();
}

std::size_t ReferenceBook::order_count() const {
    return orders_.size();
}

// Design §9. Invariants 3 and 6 hold by construction here: this book stores no per-level total
// and computes leaves() as total - filled. They become real checks in the fast book, which caches
// those numbers.
bool ReferenceBook::invariants_hold() const {
    // 1. Not crossed.
    if (!bids_.empty() && !asks_.empty() && !(bids_.begin()->first < asks_.begin()->first)) {
        return false;
    }

    std::size_t orders_in_levels = 0;
    const auto check_side = [&](const auto &levels, Side side) {
        for (const auto &[price, queue] : levels) {
            if (queue.empty()) {
                return false; // 2. no empty levels
            }
            std::optional<std::uint64_t> previous_seq;
            for (const Order &order : queue) {
                if (order.side != side || order.price != price) {
                    return false; // 4. on the right side, at its own price
                }
                if (order.leaves() <= Qty{0} || order.filled < Qty{0}) {
                    return false; // 2. no resting order with qty <= 0
                }
                if (previous_seq && order.seq <= *previous_seq) {
                    return false; // 5. FIFO: seq strictly increases front to back
                }
                previous_seq = order.seq;

                const auto found = orders_.find(order.id);
                if (found == orders_.end() || &*found->second.it != &order) {
                    return false; // 4. the lookup points at exactly this order
                }
                ++orders_in_levels;
            }
        }
        return true;
    };

    if (!check_side(bids_, Side::Buy) || !check_side(asks_, Side::Sell)) {
        return false;
    }
    // 4. Every id in the lookup is in exactly one level: counts match, and each level order
    //    was found in the lookup above.
    return orders_in_levels == orders_.size();
}

} // namespace nanobook

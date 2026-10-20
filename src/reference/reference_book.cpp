/*
 * reference_book.cpp: how the reference book handles each input.
 *
 * What it does: Implements handle() for NewOrder, Cancel and Modify, the matching loop, the checks
 *   (in the order the contract fixes), apply() for the four feed inputs, and the queries and
 *   invariant checks the tests use.
 *
 * How it works: handle(NewOrder) = validate, emit New, FOK look-first, match, then rest the
 *   remainder (Limit) or cancel it (IOC, Market). match_against() is one loop for both sides,
 *   written as a template because bids and asks are maps sorted in opposite directions.
 *   invariants_hold() walks the whole book and re-checks every rule in design §9 (slow, so
 *   tests only).
 *
 * Algorithms: Matching repeatedly takes the best level (map.begin()), trades with the front of its
 *   queue (O(1) per order), and erases a level the moment it empties. Look-first walks levels
 *   best-first and stops as soon as it has counted enough. Every count skips the trader's own
 *   orders (self-match prevention).
 *
 * Trade-offs: Clear before fast: one obvious code path per rule, and asserts that stop the program
 *   in Debug builds instead of hiding a broken invariant.
 */

#include "nanobook/reference/reference_book.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>

namespace nanobook {

namespace {

// Can an incoming order on `side` with limit `limit` trade against a resting order at `level`?
// A buy accepts asks at or below its limit; a sell accepts bids at or above its limit.
bool acceptable(Side side, Price limit, Price level) {
    return side == Side::Buy ? level <= limit : level >= limit;
}

// A Market order has no price, so it accepts every level: give it the most extreme limit.
// Every other type uses its own price.
Price effective_limit(const NewOrder &order) {
    if (order.type != OrderType::Market) {
        return order.price;
    }
    return order.side == Side::Buy ? Price{std::numeric_limits<std::int64_t>::max()}
                                   : Price{std::numeric_limits<std::int64_t>::min()};
}

// "Look first" (design §5, §6): how much could an order from `owner` with this side and limit
// actually trade right now? The owner's own resting orders don't count: self-match prevention
// would cancel them, not trade them. Read-only. Stops counting as soon as it reaches `enough`.
template <typename Levels>
Qty available_qty(const Levels &opposite, OwnerId owner, Side side, Price limit, Qty enough) {
    Qty total{};
    for (const auto &[price, queue] : opposite) { // best price first
        if (!acceptable(side, limit, price)) {
            break; // every later level is worse
        }
        for (const auto &resting : queue) {
            if (resting.owner == owner) {
                continue;
            }
            total += resting.leaves();
            if (total >= enough) {
                return total;
            }
        }
    }
    return total;
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

    // The exchange's counter only goes up (design §4), so a smaller id means a broken caller.
    assert(order.id > last_id_ && "ids reaching a book must increase");
    last_id_ = order.id;

    if (const std::optional<RejectReason> reason = validate(order)) {
        out.push(Event::make_rejected(order.id, *reason));
        return; // a rejected input changes nothing in the book (design §4)
    }

    out.push(Event::make_new(order.id));

    const Price limit = effective_limit(order);

    // FOK: look first, then act. If it can't fill completely, kill it and leave the book
    // untouched, so there is never anything to undo.
    if (order.type == OrderType::FOK) {
        const Qty available = order.side == Side::Buy
                                  ? available_qty(asks_, order.owner, order.side, limit, order.qty)
                                  : available_qty(bids_, order.owner, order.side, limit, order.qty);
        if (available < order.qty) {
            out.push(Event::make_canceled(order.id, order.qty, CancelReason::FokKill));
            return;
        }
    }

    const Qty remaining = match(order.id, order.owner, order.side, limit, order.qty, out);
    if (remaining == Qty{0}) {
        return;
    }

    assert(order.type != OrderType::FOK && "FOK passed the look-first check, so it must fill");

    // Only a Limit order rests. IOC and Market cancel whatever is left (design §5).
    if (order.type != OrderType::Limit) {
        out.push(Event::make_canceled(order.id, remaining, CancelReason::Unfilled));
        return;
    }

    rest(Order{.id = order.id,
               .owner = order.owner,
               .side = order.side,
               .price = order.price,
               .total = order.qty,
               .filled = order.qty - remaining,
               .seq = next_seq_++});
}

void ReferenceBook::handle(const Cancel &cancel, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    const auto found = orders_.find(cancel.id);
    if (found == orders_.end()) {
        // Not resting here. Whether it finished or never lived here, only the exchange knows.
        out.push(Event::make_rejected(cancel.id, RejectReason::UnknownOrder));
        return;
    }

    const Qty remaining = found->second.it->leaves();
    unlink(found);
    out.push(Event::make_canceled(cancel.id, remaining, CancelReason::Requested));
}

// Design §7. `new_total_qty` is the new TOTAL size, including what already traded.
void ReferenceBook::handle(const Modify &modify, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    const auto found = orders_.find(modify.id);
    if (found == orders_.end()) {
        out.push(Event::make_rejected(modify.id, RejectReason::UnknownOrder)); // step 2 (book)
        return;
    }

    const Order current = *found->second.it; // a copy: unlink() below destroys the original
    if (const std::optional<RejectReason> reason = validate(modify, current)) {
        out.push(Event::make_rejected(modify.id, *reason)); // steps 3-6
        return;
    }

    out.push(Event::make_replaced(modify.id, modify.new_price, modify.new_total_qty));

    // Step 7: same price, smaller total. Asking for less hurts nobody behind you, so the order
    // keeps its place in the queue. Step 5 guarantees something is still left to trade.
    if (modify.new_price == current.price && modify.new_total_qty < current.total) {
        found->second.it->total = modify.new_total_qty;
        return;
    }

    // Step 8: bigger total or new price. The order loses its place: take it out of the book
    // (freeing its slot, so this can never be BookFull), then treat it like a new Limit order
    // at the new price. It matches first if it now crosses, with self-match prevention, and
    // whatever is left rests at the back of the new price's queue.
    unlink(found);
    const Qty to_trade = modify.new_total_qty - current.filled;
    const Qty remaining =
        match(current.id, current.owner, current.side, modify.new_price, to_trade, out);
    if (remaining > Qty{0}) {
        rest(Order{.id = current.id,
                   .owner = current.owner,
                   .side = current.side,
                   .price = modify.new_price,
                   .total = modify.new_total_qty,
                   .filled = modify.new_total_qty - remaining,
                   .seq = next_seq_++});
    }
}

// ---------------------------------------------------------------------------
// Feed inputs (ITCH design §6.1). Every check comes before any change, so a failed input leaves
// the book untouched. The order of the checks is part of the contract: both books follow it.
// ---------------------------------------------------------------------------

FeedResult ReferenceBook::apply(const FeedAdd &add) {
    if (add.qty <= Qty{0} || add.qty > config_.max_qty) {
        return FeedResult::InvalidQty;
    }
    if (!config_.in_band(add.price)) {
        return FeedResult::InvalidPrice;
    }
    if (orders_.contains(add.id)) {
        return FeedResult::DuplicateOrder;
    }
    if (orders_.size() >= config_.max_orders) {
        return FeedResult::BookFull;
    }
    // No matching, even if the price crosses: the other exchange has already matched.
    rest(Order{.id = add.id,
               .owner = OwnerId{},
               .side = add.side,
               .price = add.price,
               .total = add.qty,
               .filled = Qty{0},
               .seq = next_seq_++});
    return FeedResult::Ok;
}

FeedResult ReferenceBook::apply(const FeedReduce &reduce) {
    const auto found = orders_.find(reduce.id);
    if (found == orders_.end()) {
        return FeedResult::UnknownOrder;
    }
    if (reduce.qty <= Qty{0}) {
        return FeedResult::InvalidQty;
    }
    Order &order = *found->second.it;
    if (reduce.qty > order.leaves()) {
        return FeedResult::ReduceTooLarge;
    }
    if (reduce.qty == order.leaves()) {
        unlink(found); // nothing left: it leaves the book
    } else {
        order.total -= reduce.qty; // smaller, same place in the queue
    }
    return FeedResult::Ok;
}

FeedResult ReferenceBook::apply(const FeedRemove &remove) {
    const auto found = orders_.find(remove.id);
    if (found == orders_.end()) {
        return FeedResult::UnknownOrder;
    }
    unlink(found);
    return FeedResult::Ok;
}

FeedResult ReferenceBook::apply(const FeedReplace &replace) {
    const auto found = orders_.find(replace.old_id);
    if (found == orders_.end()) {
        return FeedResult::UnknownOrder;
    }
    if (replace.qty <= Qty{0} || replace.qty > config_.max_qty) {
        return FeedResult::InvalidQty;
    }
    if (!config_.in_band(replace.price)) {
        return FeedResult::InvalidPrice;
    }
    if (replace.new_id != replace.old_id && orders_.contains(replace.new_id)) {
        return FeedResult::DuplicateOrder;
    }
    const Side side = found->second.side; // a replace keeps the side (ITCH spec)
    unlink(found);                        // frees a place, so this can't be BookFull
    rest(Order{.id = replace.new_id,
               .owner = OwnerId{},
               .side = side,
               .price = replace.price,
               .total = replace.qty,
               .filled = Qty{0},
               .seq = next_seq_++}); // the back of the queue: it lost its time priority
    return FeedResult::Ok;
}

// ---------------------------------------------------------------------------
// Checks (design §4: the order of these checks is part of the contract)
// ---------------------------------------------------------------------------

std::optional<RejectReason> ReferenceBook::validate(const NewOrder &order) const {
    if (order.qty <= Qty{0} || order.qty > config_.max_qty) {
        return RejectReason::InvalidQty;
    }
    if (order.type != OrderType::Market && !config_.in_band(order.price)) {
        return RejectReason::InvalidPrice; // a Market order's price is ignored
    }
    const Price limit = effective_limit(order);
    if (order.type == OrderType::Market) {
        // Nothing to trade against, counting only other owners' orders. Rejecting up front
        // leaves the trader's own resting orders untouched (design §6).
        const Qty available = order.side == Side::Buy
                                  ? available_qty(asks_, order.owner, order.side, limit, Qty{1})
                                  : available_qty(bids_, order.owner, order.side, limit, Qty{1});
        if (available == Qty{0}) {
            return RejectReason::NoLiquidity;
        }
    }
    // Only a Limit order can rest, and "full" only matters if it would rest without matching
    // anything (design §4). Crossing only the owner's own orders still frees a slot, because
    // self-match prevention cancels them.
    if (order.type == OrderType::Limit && orders_.size() >= config_.max_orders &&
        !crosses_book(order.side, limit)) {
        return RejectReason::BookFull;
    }
    return std::nullopt;
}

// Design §7 steps 3-6, in contract order. Steps 1-2 (unknown or finished) are handled by the
// caller, because they need the lookup.
std::optional<RejectReason> ReferenceBook::validate(const Modify &modify,
                                                    const Order &current) const {
    if (modify.new_total_qty == Qty{0}) {
        return RejectReason::UseCancel; // removal has exactly one path: Cancel
    }
    if (modify.new_total_qty < Qty{0} || modify.new_total_qty > config_.max_qty) {
        return RejectReason::InvalidQty;
    }
    if (!config_.in_band(modify.new_price)) {
        return RejectReason::InvalidPrice;
    }
    if (modify.new_total_qty <= current.filled) {
        return RejectReason::TooLateToCancel; // would leave nothing to trade: never overfill
    }
    if (modify.new_price == current.price && modify.new_total_qty == current.total) {
        return RejectReason::NoChange;
    }
    return std::nullopt;
}

bool ReferenceBook::crosses_book(Side side, Price limit) const {
    if (side == Side::Buy) {
        return !asks_.empty() && acceptable(side, limit, asks_.begin()->first);
    }
    return !bids_.empty() && acceptable(side, limit, bids_.begin()->first);
}

// ---------------------------------------------------------------------------
// Matching (design §5): best price first, oldest first within a price, trade at resting price
// ---------------------------------------------------------------------------

Qty ReferenceBook::match(OrderId aggressor, OwnerId owner, Side side, Price limit, Qty qty,
                         EventBuffer &out) {
    return side == Side::Buy ? match_against(asks_, aggressor, owner, side, limit, qty, out)
                             : match_against(bids_, aggressor, owner, side, limit, qty, out);
}

void ReferenceBook::rest(const Order &order) {
    if (order.side == Side::Buy) {
        insert(bids_, order);
    } else {
        insert(asks_, order);
    }
}

void ReferenceBook::unlink(Lookup::iterator found) {
    const Locator where = found->second;
    orders_.erase(found);
    if (where.side == Side::Buy) {
        remove(bids_, where);
    } else {
        remove(asks_, where);
    }
}

template <typename Levels>
Qty ReferenceBook::match_against(Levels &opposite, OrderId aggressor, OwnerId owner, Side side,
                                 Price limit, Qty remaining, EventBuffer &out) {
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

            // Self-match prevention (design §6): never trade with yourself. Cancel the owner's
            // own resting order (it is stale) and keep matching against the next one.
            if (resting.owner == owner) {
                out.push(
                    Event::make_canceled(resting.id, resting.leaves(), CancelReason::SelfMatch));
                orders_.erase(resting.id);
                queue.pop_front();
                continue;
            }

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
    orders_.emplace(
        order.id, Locator{.side = order.side, .price = order.price, .it = std::prev(queue.end())});
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

std::vector<OrderId> ReferenceBook::resting_ids() const {
    std::vector<OrderId> ids;
    ids.reserve(orders_.size());
    for (const auto &entry : orders_) {
        ids.push_back(entry.first);
    }
    return ids;
}

// Design §9. Invariants 3 and 6 hold by construction here: this book stores no per-level total
// and computes leaves() as total - filled. They become real checks in the fast book, which caches
// those numbers.
bool ReferenceBook::invariants_hold() const {
    // 1. Not crossed.
    if (!bids_.empty() && !asks_.empty() && !(bids_.begin()->first < asks_.begin()->first)) {
        return false;
    }
    return structure_holds();
}

// Every invariant except "not crossed" (design §9 2-6). A copy of another exchange's book must
// accept a crossed book if that exchange's book was crossed (ITCH design §8.1).
bool ReferenceBook::structure_holds() const {
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

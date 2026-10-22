/*
 * fast_book.hpp: the fast order book (phase 2): the same behaviour as the reference book, built
 *   for speed.
 *
 * What it does: Exactly what the reference book does, event for event: price-time matching for
 *   Limit, Market, IOC and FOK orders, Cancel, Modify and self-match prevention, following the
 *   same contract (docs/design.md). The differential fuzzer and the typed tests check that. As a
 *   copy of another exchange's book (phase 4) it applies feed inputs instead: add without
 *   matching, reduce, remove, replace (ITCH design §6.1), again exactly like the reference book.
 *
 * Why it exists: The reference book allocates about twice per added order and scatters its data
 *   across memory. This book keeps every order in a pool allocated once (OrderPool), links each
 *   price level's orders through the orders themselves (IntrusiveQueue), and caches each level's
 *   total quantity, so a cancel or a fill never needs a new memory request for its order.
 *
 * How it works: Matching takes the best opposite level, trades with the front of its queue and
 *   moves on, cancelling the trader's own resting orders instead of trading with them. A
 *   resting order is found by id through the id index, then unlinked from its queue in O(1). A
 *   modify that keeps priority shrinks the order in place; one that loses it is unlinked, matched
 *   like a new order, and the rest goes to the back of the new price's queue, keeping its slot.
 *
 * Data structures: OrderPool (all order slots, free list); one IntrusiveQueue per price level,
 *   inside Levels; IdTable (id -> slot, a flat hash table). Levels is a template parameter, so
 *   different price-level structures can be compared with identical book code (design §6).
 *
 * Trade-offs: FastBook uses SortedLevels (works for any price band); TickFastBook uses the
 *   TickLevels experiment (narrow bands only), kept to measure against it (design §6). After
 *   construction neither allocates while trading. On purpose this file shares no logic with the
 *   reference book: if both used one buggy helper they would agree, and the fuzzer, which looks
 *   for differences, could never notice.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/fast/id_table.hpp"
#include "nanobook/fast/intrusive_queue.hpp"
#include "nanobook/fast/level.hpp"
#include "nanobook/fast/order.hpp"
#include "nanobook/fast/order_pool.hpp"
#include "nanobook/fast/sorted_levels.hpp"
#include "nanobook/fast/tick_levels.hpp"

namespace nanobook::fast {

template <typename Levels> class BasicFastBook {
public:
    explicit BasicFastBook(BookConfig config)
        : config_(config), pool_(config.max_orders), levels_(config), ids_(config.max_orders) {
        assert(config_.is_valid());
    }

    // The bytes this book's arrays (pool, levels, id table) take from the memory resource that
    // is the default while it is built: the timed tools size their huge-page arena with it
    // (optimise design E6). SortedLevels only: TickLevels keeps plain std::vector.
    [[nodiscard]] static std::size_t arena_bytes(const BookConfig &config)
        requires std::same_as<Levels, SortedLevels>
    {
        return OrderPool::arena_bytes(config.max_orders) + SortedLevels::arena_bytes(config) +
               IdTable::arena_bytes(config.max_orders);
    }

    // Each call clears `out`, then writes this input's events (design §4). `out` must hold at
    // least config.max_events_per_input() events.
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
    [[nodiscard]] std::optional<Price> best_bid() const {
        const Level *level = levels_.best(Side::Buy);
        return level == nullptr ? std::nullopt : std::optional<Price>{level->price};
    }
    [[nodiscard]] std::optional<Price> best_ask() const {
        const Level *level = levels_.best(Side::Sell);
        return level == nullptr ? std::nullopt : std::optional<Price>{level->price};
    }
    [[nodiscard]] Qty level_qty(Side side, Price price) const {
        const Level *level = levels_.find(side, price);
        return level == nullptr ? Qty{} : level->total;
    }
    [[nodiscard]] std::vector<OrderId> level_orders(Side side, Price price) const;
    // Up to max_levels levels, best first: (price, cached total qty).
    [[nodiscard]] std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t max_levels) const;
    // How many levels a new or vanishing level at `price` shifts (SortedLevels only: a tick array
    // shifts nothing). For the real-flow analysis (ITCH design step 6), not the hot path.
    [[nodiscard]] std::size_t levels_better_than(Side side, Price price) const
        requires requires(const Levels &levels) { levels.better_than(side, price); }
    {
        return levels_.better_than(side, price);
    }
    [[nodiscard]] std::optional<Qty> leaves(OrderId id) const {
        const std::uint32_t slot = ids_.find(id);
        return slot == kNone ? std::nullopt : std::optional<Qty>{pool_[slot].leaves};
    }
    [[nodiscard]] std::size_t order_count() const {
        return pool_.used();
    }
    [[nodiscard]] std::vector<OrderId> resting_ids() const; // ascending
    [[nodiscard]] bool invariants_hold() const;             // fast-book design §8.5
    [[nodiscard]] bool structure_holds() const;             // the same, minus "not crossed"

private:
    // Small rule helpers. Written again here, not shared with the reference book (see above).
    static Side opposite(Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }
    // Can an order on `side` with limit `limit` trade with a resting order at `level`?
    static bool acceptable(Side side, Price limit, Price level) {
        return side == Side::Buy ? level <= limit : level >= limit;
    }
    // A Market order has no price: give it the most extreme limit, so every level is acceptable.
    static Price effective_limit(const NewOrder &order) {
        if (order.type != OrderType::Market) {
            return order.price;
        }
        return order.side == Side::Buy ? Price{std::numeric_limits<std::int64_t>::max()}
                                       : Price{std::numeric_limits<std::int64_t>::min()};
    }

    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder &order) const;
    [[nodiscard]] std::optional<RejectReason> validate(const Modify &modify,
                                                       const Order &current) const;
    [[nodiscard]] bool crosses_book(Side side, Price limit) const;
    [[nodiscard]] Qty available_qty(OwnerId owner, Side side, Price limit, Qty enough) const;

    Qty match(OrderId aggressor, OwnerId owner, Side side, Price limit, Qty qty, EventBuffer &out);
    void link_into_level(std::uint32_t slot);
    void remove_resting(Level &level, std::uint32_t slot);
    void remove_and_tidy(std::uint32_t slot);

    BookConfig config_;
    OrderPool pool_;
    Levels levels_;
    IdTable ids_;       // resting orders only
    OrderId last_id_{}; // ids reaching a book must increase (design §4)
};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

template <typename Levels>
void BasicFastBook<Levels>::handle(const NewOrder &order, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    assert(order.id > last_id_ && "ids reaching a book must increase");
    last_id_ = order.id;

    if (const std::optional<RejectReason> reason = validate(order)) {
        out.push(Event::make_rejected(order.id, *reason));
        return;
    }
    out.push(Event::make_new(order.id));

    const Price limit = effective_limit(order);
    if (order.type == OrderType::FOK &&
        available_qty(order.owner, order.side, limit, order.qty) < order.qty) {
        out.push(Event::make_canceled(order.id, order.qty, CancelReason::FokKill));
        return; // look first, then act: nothing was touched
    }

    const Qty remaining = match(order.id, order.owner, order.side, limit, order.qty, out);
    if (remaining == Qty{0}) {
        return;
    }
    assert(order.type != OrderType::FOK && "FOK passed the look-first check, so it must fill");
    if (order.type != OrderType::Limit) {
        out.push(Event::make_canceled(order.id, remaining, CancelReason::Unfilled));
        return;
    }

    // Rest the remainder. validate() rejected a full book unless the order crossed, and an order
    // that crossed and still has a remainder freed at least one slot (design §4), so this works.
    const std::uint32_t slot = pool_.acquire();
    assert(slot != kNone);
    Order &resting = pool_[slot];
    resting.id = order.id;
    resting.leaves = remaining;
    resting.filled = order.qty - remaining;
    resting.price = order.price;
    resting.owner = order.owner;
    resting.side = order.side;
    link_into_level(slot);
    ids_.insert(order.id, slot);
}

template <typename Levels>
void BasicFastBook<Levels>::handle(const Cancel &cancel, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    const std::uint32_t slot = ids_.find(cancel.id);
    if (slot == kNone) {
        out.push(Event::make_rejected(cancel.id, RejectReason::UnknownOrder));
        return;
    }
    const Order &order = pool_[slot];
    const Qty remaining = order.leaves;
    const Side side = order.side;
    const Price price = order.price;

    Level *level = levels_.find(side, price);
    assert(level != nullptr);
    remove_resting(*level, slot);
    if (level->queue.empty()) {
        levels_.erase(side, price); // no empty levels (design §9)
    }
    out.push(Event::make_canceled(cancel.id, remaining, CancelReason::Requested));
}

// Design §7. new_total_qty is the new TOTAL, including what already traded.
template <typename Levels>
void BasicFastBook<Levels>::handle(const Modify &modify, EventBuffer &out) {
    assert(out.capacity() >= config_.max_events_per_input());
    out.clear();

    const std::uint32_t slot = ids_.find(modify.id);
    if (slot == kNone) {
        out.push(Event::make_rejected(modify.id, RejectReason::UnknownOrder));
        return;
    }
    Order &order = pool_[slot]; // the pool never resizes, so this reference stays valid
    if (const std::optional<RejectReason> reason = validate(modify, order)) {
        out.push(Event::make_rejected(modify.id, *reason));
        return;
    }
    out.push(Event::make_replaced(modify.id, modify.new_price, modify.new_total_qty));

    const Qty old_total = order.leaves + order.filled;
    Level *level = levels_.find(order.side, order.price);
    assert(level != nullptr);

    // Step 7: same price, smaller total. It keeps its place; only the quantities shrink.
    if (modify.new_price == order.price && modify.new_total_qty < old_total) {
        const Qty shrink = old_total - modify.new_total_qty;
        order.leaves -= shrink;
        level->total -= shrink;
        return;
    }

    // Step 8: it loses its place. Take it out of its queue but keep its slot and its id entry
    // (it is about to rest again, or be removed if it fills completely), then match it like a
    // new order at the new price.
    level->total -= order.leaves;
    level->queue.unlink(pool_, slot);
    if (level->queue.empty()) {
        levels_.erase(order.side, order.price);
    }

    const Qty to_trade = modify.new_total_qty - order.filled;
    const Qty remaining = match(order.id, order.owner, order.side, modify.new_price, to_trade, out);
    if (remaining == Qty{0}) {
        ids_.erase(order.id);
        pool_.release(slot);
        return;
    }
    order.price = modify.new_price;
    order.leaves = remaining;
    order.filled = modify.new_total_qty - remaining;
    link_into_level(slot); // the back of the new price's queue
}

// ---------------------------------------------------------------------------
// Feed inputs (ITCH design §6.1): the same checks, in the same order, as the reference book.
// ---------------------------------------------------------------------------

template <typename Levels> FeedResult BasicFastBook<Levels>::apply(const FeedAdd &add) {
    if (add.qty <= Qty{0} || add.qty > config_.max_qty) {
        return FeedResult::InvalidQty;
    }
    if (!config_.in_band(add.price)) {
        return FeedResult::InvalidPrice; // also keeps TickLevels' array index in range
    }
    if (ids_.find(add.id) != kNone) {
        return FeedResult::DuplicateOrder;
    }
    if (pool_.full()) {
        return FeedResult::BookFull;
    }
    // No matching, even if the price crosses: the other exchange has already matched.
    const std::uint32_t slot = pool_.acquire();
    Order &order = pool_[slot];
    order.id = add.id;
    order.leaves = add.qty;
    order.filled = Qty{0};
    order.price = add.price;
    order.owner = OwnerId{};
    order.side = add.side;
    link_into_level(slot);
    ids_.insert(add.id, slot);
    return FeedResult::Ok;
}

template <typename Levels> FeedResult BasicFastBook<Levels>::apply(const FeedReduce &reduce) {
    const std::uint32_t slot = ids_.find(reduce.id);
    if (slot == kNone) {
        return FeedResult::UnknownOrder;
    }
    if (reduce.qty <= Qty{0}) {
        return FeedResult::InvalidQty;
    }
    Order &order = pool_[slot];
    if (reduce.qty > order.leaves) {
        return FeedResult::ReduceTooLarge;
    }
    if (reduce.qty == order.leaves) {
        remove_and_tidy(slot); // nothing left: it leaves the book
        return FeedResult::Ok;
    }
    Level *level = levels_.find(order.side, order.price);
    assert(level != nullptr);
    order.leaves -= reduce.qty; // smaller, same place in the queue
    level->total -= reduce.qty;
    return FeedResult::Ok;
}

template <typename Levels> FeedResult BasicFastBook<Levels>::apply(const FeedRemove &remove) {
    const std::uint32_t slot = ids_.find(remove.id);
    if (slot == kNone) {
        return FeedResult::UnknownOrder;
    }
    remove_and_tidy(slot);
    return FeedResult::Ok;
}

template <typename Levels> FeedResult BasicFastBook<Levels>::apply(const FeedReplace &replace) {
    const std::uint32_t slot = ids_.find(replace.old_id);
    if (slot == kNone) {
        return FeedResult::UnknownOrder;
    }
    if (replace.qty <= Qty{0} || replace.qty > config_.max_qty) {
        return FeedResult::InvalidQty;
    }
    if (!config_.in_band(replace.price)) {
        return FeedResult::InvalidPrice;
    }
    if (replace.new_id != replace.old_id && ids_.find(replace.new_id) != kNone) {
        return FeedResult::DuplicateOrder;
    }
    // Take the order off its level but keep its slot (as Modify does), then put it back as the
    // new order: same side (ITCH spec), new id, price and qty, at the back of the queue.
    Order &order = pool_[slot]; // the pool never resizes, so this reference stays valid
    Level *level = levels_.find(order.side, order.price);
    assert(level != nullptr);
    level->total -= order.leaves;
    level->queue.unlink(pool_, slot);
    if (level->queue.empty()) {
        levels_.erase(order.side, order.price);
    }
    ids_.erase(replace.old_id);
    order.id = replace.new_id;
    order.price = replace.price;
    order.leaves = replace.qty;
    order.filled = Qty{0};
    link_into_level(slot);
    ids_.insert(replace.new_id, slot);
    return FeedResult::Ok;
}

// ---------------------------------------------------------------------------
// Checks, in the order the contract fixes (design §4, §7)
// ---------------------------------------------------------------------------

template <typename Levels>
std::optional<RejectReason> BasicFastBook<Levels>::validate(const NewOrder &order) const {
    if (order.qty <= Qty{0} || order.qty > config_.max_qty) {
        return RejectReason::InvalidQty;
    }
    if (order.type != OrderType::Market && !config_.in_band(order.price)) {
        return RejectReason::InvalidPrice;
    }
    const Price limit = effective_limit(order);
    if (order.type == OrderType::Market &&
        available_qty(order.owner, order.side, limit, Qty{1}) == Qty{0}) {
        return RejectReason::NoLiquidity; // nothing from other owners: own orders stay untouched
    }
    if (order.type == OrderType::Limit && pool_.full() && !crosses_book(order.side, limit)) {
        return RejectReason::BookFull;
    }
    return std::nullopt;
}

template <typename Levels>
std::optional<RejectReason> BasicFastBook<Levels>::validate(const Modify &modify,
                                                            const Order &current) const {
    if (modify.new_total_qty == Qty{0}) {
        return RejectReason::UseCancel;
    }
    if (modify.new_total_qty < Qty{0} || modify.new_total_qty > config_.max_qty) {
        return RejectReason::InvalidQty;
    }
    if (!config_.in_band(modify.new_price)) {
        return RejectReason::InvalidPrice;
    }
    if (modify.new_total_qty <= current.filled) {
        return RejectReason::TooLateToCancel;
    }
    if (modify.new_price == current.price &&
        modify.new_total_qty == current.leaves + current.filled) {
        return RejectReason::NoChange;
    }
    return std::nullopt;
}

template <typename Levels> bool BasicFastBook<Levels>::crosses_book(Side side, Price limit) const {
    const Level *best = levels_.best(opposite(side));
    return best != nullptr && acceptable(side, limit, best->price);
}

// "Look first" (design §5, §6): how much could `owner` trade right now, counting only other
// owners' orders? Read-only; stops as soon as it reaches `enough`.
template <typename Levels>
Qty BasicFastBook<Levels>::available_qty(OwnerId owner, Side side, Price limit, Qty enough) const {
    Qty total{};
    levels_.for_each_best_first(opposite(side), [&](const Level &level) {
        if (!acceptable(side, limit, level.price)) {
            return false; // every later level is worse
        }
        for (std::uint32_t s = level.queue.front(); s != kNone; s = pool_[s].next) {
            if (pool_[s].owner == owner) {
                continue; // would be cancelled by self-match prevention, not traded
            }
            total += pool_[s].leaves;
            if (total >= enough) {
                return false;
            }
        }
        return true;
    });
    return total;
}

// ---------------------------------------------------------------------------
// Matching (design §5): best price first, oldest first, trade at the resting price
// ---------------------------------------------------------------------------

template <typename Levels>
Qty BasicFastBook<Levels>::match(OrderId aggressor, OwnerId owner, Side side, Price limit, Qty qty,
                                 EventBuffer &out) {
    const Side other = opposite(side);
    Qty remaining = qty;
    while (remaining > Qty{0}) {
        Level *level = levels_.best(other);
        if (level == nullptr || !acceptable(side, limit, level->price)) {
            break;
        }
        assert(!level->queue.empty() && "empty price level left in the book");

        while (remaining > Qty{0} && !level->queue.empty()) {
            const std::uint32_t slot = level->queue.front();
            Order &resting = pool_[slot];
            if (resting.owner == owner) { // self-match prevention (design §6)
                out.push(Event::make_canceled(resting.id, resting.leaves, CancelReason::SelfMatch));
                remove_resting(*level, slot);
                continue;
            }
            const Qty traded = std::min(remaining, resting.leaves);
            out.push(Event::make_trade(aggressor, resting.id, level->price, traded));
            resting.leaves -= traded;
            resting.filled += traded;
            level->total -= traded;
            remaining -= traded;
            if (resting.leaves == Qty{0}) {
                remove_resting(*level, slot);
            }
        }

        if (level->queue.empty()) {
            levels_.erase(other, level->price);
        }
    }
    return remaining;
}

// Puts an order (its fields already set) at the back of its price level's queue.
template <typename Levels> void BasicFastBook<Levels>::link_into_level(std::uint32_t slot) {
    const Order &order = pool_[slot];
    Level &level = levels_.get_or_create(order.side, order.price);
    level.queue.push_back(pool_, slot);
    level.total += order.leaves;
}

// Takes a resting order out completely: off its queue, out of the id index, slot freed. The
// caller removes the level if this left it empty.
template <typename Levels>
void BasicFastBook<Levels>::remove_resting(Level &level, std::uint32_t slot) {
    const Order &order = pool_[slot];
    level.total -= order.leaves;
    level.queue.unlink(pool_, slot);
    ids_.erase(order.id);
    pool_.release(slot);
}

// remove_resting(), then removes the level if that left it empty.
template <typename Levels> void BasicFastBook<Levels>::remove_and_tidy(std::uint32_t slot) {
    const Side side = pool_[slot].side; // copies: remove_resting() frees the slot
    const Price price = pool_[slot].price;
    Level *level = levels_.find(side, price);
    assert(level != nullptr);
    remove_resting(*level, slot);
    if (level->queue.empty()) {
        levels_.erase(side, price); // no empty levels (design §9)
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

template <typename Levels>
std::vector<OrderId> BasicFastBook<Levels>::level_orders(Side side, Price price) const {
    std::vector<OrderId> ids;
    if (const Level *level = levels_.find(side, price)) {
        for (std::uint32_t s = level->queue.front(); s != kNone; s = pool_[s].next) {
            ids.push_back(pool_[s].id);
        }
    }
    return ids;
}

template <typename Levels>
std::vector<std::pair<Price, Qty>> BasicFastBook<Levels>::depth(Side side,
                                                                std::size_t max_levels) const {
    std::vector<std::pair<Price, Qty>> levels;
    levels_.for_each_best_first(side, [&](const Level &level) {
        if (levels.size() >= max_levels) {
            return false;
        }
        levels.emplace_back(level.price, level.total);
        return true;
    });
    return levels;
}

template <typename Levels> std::vector<OrderId> BasicFastBook<Levels>::resting_ids() const {
    std::vector<OrderId> ids;
    ids.reserve(pool_.used());
    for (const Side side : {Side::Buy, Side::Sell}) {
        levels_.for_each_best_first(side, [&](const Level &level) {
            for (std::uint32_t s = level.queue.front(); s != kNone; s = pool_[s].next) {
                ids.push_back(pool_[s].id);
            }
            return true;
        });
    }
    std::ranges::sort(ids); // same order as the reference book's
    return ids;
}

// Fast-book design §8.5: the reference book's invariants, plus the ones that exist because this
// book caches data (level totals, links, the id index, the free list).
template <typename Levels> bool BasicFastBook<Levels>::invariants_hold() const {
    const std::optional<Price> bid = best_bid();
    const std::optional<Price> ask = best_ask();
    if (bid && ask && !(*bid < *ask)) {
        return false; // crossed
    }
    return structure_holds();
}

// Every invariant except "not crossed". A copy of another exchange's book must accept a crossed
// book if that exchange's book was crossed (ITCH design §8.1).
template <typename Levels> bool BasicFastBook<Levels>::structure_holds() const {
    std::size_t queued = 0;
    bool ok = true;
    for (const Side side : {Side::Buy, Side::Sell}) {
        std::optional<Price> previous;
        levels_.for_each_best_first(side, [&](const Level &level) {
            const bool ordered = !previous || (side == Side::Buy ? level.price < *previous
                                                                 : level.price > *previous);
            if (!ordered || level.queue.empty() || !level.queue.links_consistent(pool_)) {
                ok = false; // levels best-first, none empty, every link consistent
                return false;
            }
            previous = level.price;
            Qty sum{};
            for (std::uint32_t s = level.queue.front(); s != kNone; s = pool_[s].next) {
                const Order &o = pool_[s];
                if (o.price != level.price || o.side != side || o.leaves <= Qty{0} ||
                    o.filled < Qty{0} || ids_.find(o.id) != s) {
                    ok = false; // on the right level, quantities valid, the index points back here
                    return false;
                }
                sum += o.leaves;
                ++queued;
            }
            if (sum != level.total) {
                ok = false; // the cached total matches the queue
                return false;
            }
            return true;
        });
        if (!ok) {
            return false;
        }
    }
    // Every slot in use is queued exactly once and indexed; the free list holds the rest.
    return queued == pool_.used() && ids_.size() == pool_.used() && pool_.free_list_consistent();
}

} // namespace nanobook::fast

namespace nanobook {

// The fast book as tests, the exchange, the fuzzer and the benchmarks use it: sorted levels with
// binary search (design §6.1).
using FastBook = fast::BasicFastBook<fast::SortedLevels>;

// The tick-array experiment (design §6.2): same book, levels in a tick array. Narrow bands only.
using TickFastBook = fast::BasicFastBook<fast::TickLevels>;

static_assert(FeedBook<FastBook>);
static_assert(FeedBook<TickFastBook>);

} // namespace nanobook

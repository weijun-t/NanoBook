/*
 * itch_flow.hpp: measures what real order flow does to the fast book's price levels (step 6).
 *
 * What it does: FlowAnalysis wraps a BookBuilder<FastBook> and, around each order message,
 *   records:
 *   - every price level created (an order at a new price) or removed (the last order at a price
 *     leaves), and how many levels it shifted: the levels better than it, which the sorted array
 *     moves along by one (fast-book design §6.1);
 *   - how far each new order is from its side's best price, from "improves the best" to "more
 *     than $100 away".
 *   FlowStats keeps them as counts in buckets. It also feeds every level lookup, creation and
 *   removal to three WindowModels (itch_window.hpp), W = 256, 1,024 and 4,096: how E7's moving
 *   window would fare (optimise design §10.5).
 *
 * Why it exists: Phase 3 measured the sorted array's weak spot: a new level deep in a big book
 *   costs 1.41 µs at p50. Whether real flow does that often is fast-book design §13's question,
 *   and the tick array's price ranges (step 6a) suggest it might: market makers park "stub"
 *   quotes far from the market.
 *
 * How it works: Untimed. It follows every resting order's side and price (order ref -> where, a
 *   hash map, cold path), because a book can't be asked an order's price. Before a removal: if
 *   the order is the last one at its price, the level will vanish, so the levels better than it
 *   are counted now. After an add or a replace: if the order is alone at its price, it made a
 *   new level, and the levels better than it are counted (inserting doesn't change that count).
 *   Shifts go in power-of-two buckets: 0, 1, 2-3, 4-7, ...
 *
 * Trade-offs: Analysis code: it allocates (the hash map) and adds O(log levels) queries per
 *   message, so it never runs in a timed replay.
 */

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <unordered_map>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/messages.hpp"

#include "itch_window.hpp"

namespace nanobook::itch {

inline constexpr std::size_t kShiftBuckets = 25; // up to 2^24 levels shifted

// 0 -> 0, 1 -> 1, 2-3 -> 2, 4-7 -> 3, ...: bucket b > 0 holds shifts in [2^(b-1), 2^b).
inline std::size_t shift_bucket(std::size_t shifted) {
    return std::min(static_cast<std::size_t>(std::bit_width(shifted)), kShiftBuckets - 1);
}

// How far a new order is from the best price on its side, before it arrives.
enum class Distance : std::uint8_t {
    EmptySide,    // nothing on its side yet
    ImprovesBest, // a better price than the best: it becomes the best
    AtBest,
    Within1Cent, // behind the best by less than $0.01 (100 ticks of $0.0001)
    Within10Cents,
    Within1Dollar,
    Within10Dollars,
    Within100Dollars,
    Farther,
};
inline constexpr std::size_t kDistances = 9;

// The window widths simulated (optimise design §10.5).
inline constexpr std::array<std::int64_t, 3> kWindowWidths{256, 1024, 4096};

struct FlowStats {
    std::array<std::uint64_t, kShiftBuckets> created{}; // new levels, by levels shifted
    std::array<std::uint64_t, kShiftBuckets> removed{}; // vanished levels, by levels shifted
    std::uint64_t created_shifted{};                    // total levels shifted by creations
    std::uint64_t removed_shifted{};
    std::array<std::uint64_t, kDistances> distance{}; // new resting orders, by distance from best
};

template <typename Builder> class FlowAnalysis {
public:
    explicit FlowAnalysis(Builder &inner)
        : inner_(inner), windows_{WindowModel{kWindowWidths[0], inner.locates()},
                                  WindowModel{kWindowWidths[1], inner.locates()},
                                  WindowModel{kWindowWidths[2], inner.locates()}} {
        where_.reserve(1 << 22);
    }

    void on(const AddOrder &a) {
        const std::optional<Distance> d =
            distance(a.locate, a.side == 'B' ? Side::Buy : Side::Sell, Price{a.price});
        inner_.on(a);
        rested(a.locate, a.order_ref, a.side == 'B' ? Side::Buy : Side::Sell, Price{a.price},
               a.shares, d);
    }
    void on(const OrderExecuted &e) {
        reduced(e, e.order_ref, e.executed_shares);
    }
    void on(const OrderExecutedWithPrice &c) {
        reduced(c, c.order_ref, c.executed_shares);
    }
    void on(const OrderCancel &x) {
        reduced(x, x.order_ref, x.cancelled_shares);
    }
    void on(const OrderDelete &d) {
        const auto found = where_.find(d.order_ref);
        if (found != where_.end()) {
            looked_up(found->second);
            leaving(found->second, d.order_ref);
        }
        inner_.on(d);
        forget(d.order_ref);
    }
    void on(const OrderReplace &u) {
        const auto found = where_.find(u.original_order_ref);
        std::optional<Where> old;
        std::optional<Distance> d;
        if (found != where_.end()) {
            old = found->second;
            looked_up(*old);
            leaving(*old, u.original_order_ref);
            d = distance(old->locate, old->side, Price{u.price});
        }
        inner_.on(u);
        forget(u.original_order_ref);
        if (old) {
            rested(old->locate, u.new_order_ref, old->side, Price{u.price}, u.shares, d);
        }
    }
    template <typename Message> void on(const Message &m) { // S, R, H
        inner_.on(m);
    }
    void on_skipped(char type) {
        inner_.on_skipped(type);
    }

    [[nodiscard]] const FlowStats &stats() const {
        return stats_;
    }
    [[nodiscard]] const std::array<WindowModel, kWindowWidths.size()> &windows() const {
        return windows_;
    }

private:
    struct Where {
        std::uint16_t locate{};
        Side side{Side::Buy};
        Price price{};
    };

    [[nodiscard]] bool has_book(std::uint16_t locate) const {
        return locate != 0 && locate < inner_.locates() && inner_.name(locate) != Symbol{};
    }

    // Before an order arrives: how far is `price` from its side's best?
    std::optional<Distance> distance(std::uint16_t locate, Side side, Price price) const {
        if (!has_book(locate)) {
            return std::nullopt;
        }
        const auto &book = inner_.book(locate);
        const std::optional<Price> best = side == Side::Buy ? book.best_bid() : book.best_ask();
        if (!best) {
            return Distance::EmptySide;
        }
        const std::int64_t behind =
            side == Side::Buy ? best->ticks - price.ticks : price.ticks - best->ticks;
        if (behind < 0) {
            return Distance::ImprovesBest;
        }
        if (behind == 0) {
            return Distance::AtBest;
        }
        constexpr std::array<std::pair<std::int64_t, Distance>, 5> kSteps{{
            {100, Distance::Within1Cent},
            {1'000, Distance::Within10Cents},
            {10'000, Distance::Within1Dollar},
            {100'000, Distance::Within10Dollars},
            {1'000'000, Distance::Within100Dollars},
        }};
        for (const auto &[limit, d] : kSteps) {
            if (behind < limit) {
                return d;
            }
        }
        return Distance::Farther;
    }

    // After an add or a replace: is the order resting, and alone at its price (a new level)?
    void rested(std::uint16_t locate, std::uint64_t ref, Side side, Price price,
                std::uint32_t shares, std::optional<Distance> d) {
        if (!has_book(locate)) {
            return;
        }
        const auto &book = inner_.book(locate);
        if (book.leaves(OrderId{ref}) != Qty{shares}) {
            return; // the book refused it (counted by the builder)
        }
        where_[ref] = Where{.locate = locate, .side = side, .price = price};
        looked_up(where_[ref]);
        if (d) {
            ++stats_.distance[static_cast<std::size_t>(*d)];
        }
        if (book.level_qty(side, price) == Qty{shares}) {
            const std::size_t shifted = book.levels_better_than(side, price);
            ++stats_.created[shift_bucket(shifted)];
            stats_.created_shifted += shifted;
            for (WindowModel &w : windows_) {
                w.created(locate, side, price);
            }
        }
    }

    // Before an order leaves: is it the last one at its price (the level will vanish)? It is alone
    // exactly when the level's total is its own remaining shares (every resting order has > 0).
    void leaving(const Where &w, std::uint64_t ref) {
        const auto &book = inner_.book(w.locate);
        if (book.level_qty(w.side, w.price) == book.leaves(OrderId{ref})) {
            const std::size_t shifted = book.levels_better_than(w.side, w.price);
            ++stats_.removed[shift_bucket(shifted)];
            stats_.removed_shifted += shifted;
            for (WindowModel &window : windows_) {
                window.removed(w.locate, w.side, w.price);
            }
        }
    }

    // The book looks this order's level up by price (find or get_or_create).
    void looked_up(const Where &w) {
        for (WindowModel &window : windows_) {
            window.lookup(w.locate, w.side, w.price);
        }
    }

    template <typename Message>
    void reduced(const Message &m, std::uint64_t ref, std::uint32_t shares) {
        const auto found = where_.find(ref);
        if (found != where_.end()) {
            looked_up(found->second);
        }
        if (found != where_.end() &&
            inner_.book(found->second.locate).leaves(OrderId{ref}) == Qty{shares}) {
            leaving(found->second, ref); // reduced to zero: it leaves the book
        }
        inner_.on(m);
        if (found != where_.end() && !inner_.book(found->second.locate).leaves(OrderId{ref})) {
            where_.erase(found);
        }
    }

    void forget(std::uint64_t ref) {
        where_.erase(ref);
    }

    Builder &inner_;
    std::unordered_map<std::uint64_t, Where> where_; // resting orders: ref -> stock, side, price
    FlowStats stats_;
    std::array<WindowModel, kWindowWidths.size()> windows_; // E7's window at each width
};

} // namespace nanobook::itch

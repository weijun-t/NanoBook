/*
 * itch_replay_comparison.hpp: replays one ITCH day into two kinds of book at once and compares.
 *
 * What it does: compare_books() compares two copies of one stock's book completely: resting
 *   orders, best prices, every level (price and total), every queue's order, and every order's
 *   remaining shares. ReplayComparison is a parser Handler that passes every message to two book
 *   builders, then checks the stock the message touched: quickly after every order message (order
 *   count, best bid and ask and their quantities), fully every N order messages, and fully for
 *   every stock at each System Event (S: start of day, market open and close, ...) and at the end
 *   (finish()). It counts mismatches and keeps the first one.
 *
 * Why it exists: ITCH design §8.2. Phase 2 proved the fast book against the reference book on
 *   random inputs; this proves it on a real trading day. The reference book is written for
 *   clarity, the fast book for speed, and they share no code (fast_book.hpp), so agreeing on
 *   363 million real order messages is strong evidence both are right.
 *
 * How it works: The quick check is O(log levels) per message. A full check of one stock is
 *   O(its orders); of every stock, O(all resting orders). The reference book adds each level's
 *   total up from its orders while the fast book keeps a cached total, so comparing depth() also
 *   checks that cache on every level.
 *
 * Trade-offs: Test and tool code (it allocates while comparing). After the first mismatch the
 *   two copies differ for good, so only the first is described; the rest are counted.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include "nanobook/core/names.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

// The first difference between two copies of one stock's book, or nothing.
template <FeedBook A, FeedBook B> std::optional<std::string> compare_books(const A &a, const B &b) {
    if (a.order_count() != b.order_count()) {
        return "order_count: " + std::to_string(a.order_count()) + " vs " +
               std::to_string(b.order_count());
    }
    if (a.best_bid() != b.best_bid() || a.best_ask() != b.best_ask()) {
        return std::string{"best bid or ask"};
    }
    for (const Side side : {Side::Buy, Side::Sell}) {
        const auto levels = a.depth(side, std::numeric_limits<std::size_t>::max());
        if (levels != b.depth(side, std::numeric_limits<std::size_t>::max())) {
            return std::string{name(side)} + " levels (prices or totals)";
        }
        for (const auto &[price, total] : levels) {
            const std::vector<OrderId> queue = a.level_orders(side, price);
            const std::string where = std::string{name(side)} + " " + std::to_string(price.ticks);
            if (queue != b.level_orders(side, price)) {
                return "queue order at " + where;
            }
            for (const OrderId id : queue) {
                if (a.leaves(id) != b.leaves(id)) {
                    return "remaining shares of order " + std::to_string(id.value) + " at " + where;
                }
            }
        }
    }
    return std::nullopt;
}

struct ComparisonMismatch {
    std::uint64_t message{};   // 1-based position in the file
    std::uint64_t timestamp{}; // nanoseconds since midnight
    std::uint16_t locate{};
    std::string what;
};

template <typename BuilderA, typename BuilderB> class ReplayComparison {
public:
    // Every `full_every` order messages, the touched stock's book is compared in full.
    ReplayComparison(BuilderA &a, BuilderB &b, std::uint64_t full_every)
        : a_(a), b_(b), full_every_(full_every) {
    }

    template <typename Message> void on(const Message &m) {
        ++messages_;
        a_.on(m);
        b_.on(m);
        if constexpr (std::is_same_v<Message, SystemEvent>) {
            compare_all(m.timestamp);
        } else if constexpr (kIsOrderMessage<Message>) {
            after_order_message(m.locate, m.timestamp);
        }
    }
    void on_skipped(char type) {
        ++messages_;
        a_.on_skipped(type);
        b_.on_skipped(type);
    }

    // The final comparison: every stock, and the two builders' statistics.
    void finish() {
        compare_all(last_timestamp_);
        const auto &sa = a_.stats();
        const auto &sb = b_.stats();
        if (sa.results != sb.results || sa.unknown_locate != sb.unknown_locate ||
            sa.bad_input != sb.bad_input || sa.crossed_starts != sb.crossed_starts ||
            sa.crossed_messages != sb.crossed_messages) {
            record(0, "the builders' statistics (results or crossings)");
        }
    }

    [[nodiscard]] std::uint64_t mismatches() const {
        return mismatches_;
    }
    [[nodiscard]] const std::optional<ComparisonMismatch> &first_mismatch() const {
        return first_;
    }
    [[nodiscard]] std::uint64_t quick_checks() const {
        return quick_checks_;
    }
    [[nodiscard]] std::uint64_t full_checks() const {
        return full_checks_;
    }

private:
    template <typename M>
    static constexpr bool kIsOrderMessage =
        std::is_same_v<M, AddOrder> || std::is_same_v<M, OrderExecuted> ||
        std::is_same_v<M, OrderExecutedWithPrice> || std::is_same_v<M, OrderCancel> ||
        std::is_same_v<M, OrderDelete> || std::is_same_v<M, OrderReplace>;

    void after_order_message(std::uint16_t locate, std::uint64_t timestamp) {
        last_timestamp_ = timestamp;
        if (locate == 0 || locate >= a_.locates()) {
            return; // no book: both builders counted it as unknown (compared in finish())
        }
        const auto &a = a_.book(locate);
        const auto &b = b_.book(locate);
        ++quick_checks_;
        const std::optional<Price> bid = a.best_bid();
        const std::optional<Price> ask = a.best_ask();
        if (a.order_count() != b.order_count() || bid != b.best_bid() || ask != b.best_ask() ||
            (bid && a.level_qty(Side::Buy, *bid) != b.level_qty(Side::Buy, *bid)) ||
            (ask && a.level_qty(Side::Sell, *ask) != b.level_qty(Side::Sell, *ask))) {
            record(locate, "quick check (order count, best prices or their quantities)");
            return;
        }
        if (full_every_ != 0 && ++order_messages_ % full_every_ == 0) {
            compare_one(locate);
        }
    }

    void compare_one(std::uint16_t locate) {
        ++full_checks_;
        if (const std::optional<std::string> diff =
                compare_books(a_.book(locate), b_.book(locate))) {
            record(locate, *diff);
        }
    }

    void compare_all(std::uint64_t timestamp) {
        last_timestamp_ = timestamp;
        for (std::size_t locate = 1; locate < a_.locates(); ++locate) {
            compare_one(static_cast<std::uint16_t>(locate));
        }
    }

    void record(std::uint16_t locate, const std::string &what) {
        ++mismatches_;
        if (!first_) {
            first_ = ComparisonMismatch{
                .message = messages_, .timestamp = last_timestamp_, .locate = locate, .what = what};
        }
    }

    BuilderA &a_;
    BuilderB &b_;
    std::uint64_t full_every_;
    std::uint64_t messages_{};
    std::uint64_t order_messages_{};
    std::uint64_t last_timestamp_{};
    std::uint64_t quick_checks_{};
    std::uint64_t full_checks_{};
    std::uint64_t mismatches_{};
    std::optional<ComparisonMismatch> first_;
};

} // namespace nanobook::itch

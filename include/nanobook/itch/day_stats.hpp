/*
 * day_stats.hpp (itch): counts what each stock needed during one ITCH day, to size its book.
 *
 * What it does: DayCounter is a parser Handler. Run over a whole day, it records for every stock
 *   the most orders that were resting at once, the lowest and highest order price, and the
 *   largest order. stats() returns them keyed by stock symbol; directory() returns the day's
 *   locate -> symbol table from the Stock Directory (R) messages.
 *
 * Why it exists: Every book reserves all its memory when it's built (rule 1), and stocks differ
 *   hugely (ITCH design §7). Counting first gives each stock exactly the room it needs.
 *   Stats are keyed by symbol, not locate, because Nasdaq renumbers locates every day: a later
 *   version can size today's books from yesterday's stats plus a margin (sizing.hpp).
 *
 * How it works: One pass, O(messages). It follows every live order: order ref -> (locate,
 *   shares) in a hash map. Add: insert, the stock's live count goes up. Execute or partial cancel:
 *   shares go down, and the order is gone at 0. Delete: gone. Replace: the old one is gone, the
 *   new one is added. Each stock's peak is the highest its live count reached.
 *
 * Data structures: std::unordered_map for live orders (reserved up front), and vectors indexed by
 *   locate that grow as locates appear.
 *
 * Trade-offs: Cold path: it allocates freely and runs before the replay, never during it. It
 *   doesn't check the feed: a reduce of an unknown order or of more than is left is just counted
 *   (as `ignored`); proving the feed consistent is the book builder's job.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

struct StockStats {
    std::uint32_t peak_orders{}; // most orders resting at once
    std::uint32_t min_price{};   // lowest order price (1/10,000 of a dollar); 0 if no orders
    std::uint32_t max_price{};
    std::uint32_t max_shares{}; // largest single order

    bool operator==(const StockStats &) const = default;
};

class DayCounter {
public:
    explicit DayCounter(std::size_t expected_live_orders = 1 << 20) {
        live_.reserve(expected_live_orders);
    }

    void on(const SystemEvent & /*unused*/) {
    }
    void on(const StockDirectory &r) {
        // Only R creates a stock: an order for a locate R never named gets no book (it is
        // counted as unknown by the book builder instead).
        if (r.locate >= directory_.size()) {
            directory_.resize(std::size_t{r.locate} + 1);
        }
        directory_[r.locate] = r.stock; // a re-sent R for a known locate is an update (§3.5)
    }
    void on(const StockTradingAction & /*unused*/) {
    }
    void on(const AddOrder &a) {
        add(a.locate, a.order_ref, a.shares, a.price);
    }
    void on(const OrderExecuted &e) {
        reduce({.ref = e.order_ref, .shares = e.executed_shares});
    }
    void on(const OrderExecutedWithPrice &c) {
        reduce({.ref = c.order_ref, .shares = c.executed_shares});
    }
    void on(const OrderCancel &x) {
        reduce({.ref = x.order_ref, .shares = x.cancelled_shares});
    }
    void on(const OrderDelete &d) {
        const auto found = live_.find(d.order_ref);
        if (found == live_.end()) {
            ++ignored_;
            return;
        }
        remove(found);
    }
    void on(const OrderReplace &u) {
        const auto found = live_.find(u.original_order_ref);
        if (found == live_.end()) {
            ++ignored_;
            return;
        }
        const std::uint16_t locate = found->second.locate;
        remove(found);
        add(locate, u.new_order_ref, u.shares, u.price);
    }
    void on_skipped(char /*type*/) {
    }

    // Locate -> symbol, as today's Stock Directory messages gave it. Index 0 is unused.
    [[nodiscard]] const std::vector<Symbol> &directory() const {
        return directory_;
    }
    // Per stock symbol, for every stock that appeared in the directory.
    [[nodiscard]] std::map<Symbol, StockStats> stats() const {
        std::map<Symbol, StockStats> by_symbol;
        for (std::size_t locate = 1; locate < directory_.size(); ++locate) {
            if (directory_[locate] != Symbol{}) {
                by_symbol[directory_[locate]] =
                    locate < stocks_.size() ? stocks_[locate] : StockStats{};
            }
        }
        return by_symbol;
    }
    // Reduces, deletes and replaces of an order that wasn't live, and adds of one that was.
    [[nodiscard]] std::uint64_t ignored() const {
        return ignored_;
    }

private:
    struct Live {
        std::uint16_t locate{};
        std::uint32_t shares{};
    };

    void grow(std::uint16_t locate) {
        if (locate >= stocks_.size()) {
            stocks_.resize(std::size_t{locate} + 1);
            live_count_.resize(std::size_t{locate} + 1);
        }
    }
    void add(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares, std::uint32_t price) {
        if (!live_.try_emplace(ref, Live{.locate = locate, .shares = shares}).second) {
            ++ignored_; // already live
            return;
        }
        grow(locate);
        StockStats &s = stocks_[locate];
        const std::uint32_t live = ++live_count_[locate];
        s.peak_orders = std::max(s.peak_orders, live);
        s.min_price = s.min_price == 0 ? price : std::min(s.min_price, price);
        s.max_price = std::max(s.max_price, price);
        s.max_shares = std::max(s.max_shares, shares);
    }
    // Named fields, not two numbers in a row, so they can't be swapped by mistake.
    struct Reduction {
        std::uint64_t ref{};
        std::uint32_t shares{}; // taken off
    };
    void reduce(Reduction r) {
        const auto found = live_.find(r.ref);
        if (found == live_.end()) {
            ++ignored_;
            return;
        }
        if (r.shares >= found->second.shares) {
            remove(found); // nothing left
        } else {
            found->second.shares -= r.shares;
        }
    }
    void remove(std::unordered_map<std::uint64_t, Live>::iterator found) {
        --live_count_[found->second.locate];
        live_.erase(found);
    }

    std::unordered_map<std::uint64_t, Live> live_; // order ref -> where it rests and its shares
    std::vector<Symbol> directory_;                // by locate
    std::vector<StockStats> stocks_;               // by locate
    std::vector<std::uint32_t> live_count_;        // by locate: orders resting right now
    std::uint64_t ignored_{};
};

} // namespace nanobook::itch

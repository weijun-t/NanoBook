/*
 * map_levels.hpp: the fast book's price levels, TEMPORARY version (a std::map per side).
 *
 * What it does: Keeps both sides' price levels, each with its queue and its total quantity, and
 *   answers what the book needs: the best level, the level at a price, a level to add an order to,
 *   removing an empty level, and walking a side best price first.
 *
 * Why it exists: Phase 2 builds the fast book "correct first" (fast-book design §11, step 4): the
 *   new parts (pool, intrusive queues, cached totals) go in while the price levels stay a simple,
 *   trusted structure. Step 6 adds SortedLevels and TickLevels with exactly this interface, and
 *   this file is then removed. Swapping one part at a time means a failure always points at the
 *   part just changed.
 *
 * How it works: Bids are a std::map sorted high to low, asks a std::map sorted low to high, so
 *   begin() is always the best level. Lookup, insert and erase are O(log n). A Level holds its
 *   price, its total remaining quantity and its IntrusiveQueue; the orders are in the OrderPool.
 *
 * Data structures: Level (32 bytes: price, total, queue). Two std::map<Price, Level>: red-black
 *   trees, one heap node per level.
 *
 * Trade-offs: Allocates a tree node per new level and scatters levels in memory, the very thing
 *   phase 2 removes, which is why it is temporary. A Level* stays valid until that level is erased.
 *   (The later level structures will only promise it until the next get_or_create or erase.)
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <type_traits>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/intrusive_queue.hpp"

namespace nanobook::fast {

struct Level {
    Price price{};
    Qty total{}; // sum of `leaves` of the orders in the queue, kept up to date by the book
    IntrusiveQueue queue{};
};

static_assert(sizeof(Level) == 32 && std::is_trivially_copyable_v<Level>);

class MapLevels {
public:
    explicit MapLevels(const BookConfig & /*config*/) {
    }

    // The level at `price`, or nullptr if there is none.
    [[nodiscard]] Level *find(Side side, Price price) {
        return side == Side::Buy ? find_in(bids_, price) : find_in(asks_, price);
    }
    [[nodiscard]] const Level *find(Side side, Price price) const {
        return side == Side::Buy ? find_in(bids_, price) : find_in(asks_, price);
    }

    // The best level of a side (highest bid, lowest ask), or nullptr if the side is empty.
    [[nodiscard]] Level *best(Side side) {
        return side == Side::Buy ? best_in(bids_) : best_in(asks_);
    }
    [[nodiscard]] const Level *best(Side side) const {
        return side == Side::Buy ? best_in(bids_) : best_in(asks_);
    }

    // The level at `price`, created empty if it doesn't exist yet.
    [[nodiscard]] Level &get_or_create(Side side, Price price) {
        return side == Side::Buy ? get_or_create_in(bids_, price) : get_or_create_in(asks_, price);
    }

    // Removes the level at `price`. It must exist and be empty.
    void erase(Side side, Price price) {
        if (side == Side::Buy) {
            erase_in(bids_, price);
        } else {
            erase_in(asks_, price);
        }
    }

    // Calls visit(level) for each level of a side, best price first, until visit returns false.
    template <typename Visit> void for_each_best_first(Side side, Visit &&visit) const {
        if (side == Side::Buy) {
            walk(bids_, visit);
        } else {
            walk(asks_, visit);
        }
    }

private:
    // Return types are written out (not `auto`): a deduced return type can't be used before the
    // compiler has seen the function's body, which GCC rejects here and Clang 18 crashes on.
    template <typename Map> static Level *find_in(Map &levels, Price price) {
        const auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }
    template <typename Map> static const Level *find_in(const Map &levels, Price price) {
        const auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }
    template <typename Map> static Level *best_in(Map &levels) {
        return levels.empty() ? nullptr : &levels.begin()->second;
    }
    template <typename Map> static const Level *best_in(const Map &levels) {
        return levels.empty() ? nullptr : &levels.begin()->second;
    }
    template <typename Map> static Level &get_or_create_in(Map &levels, Price price) {
        const auto [it, inserted] = levels.try_emplace(price);
        if (inserted) {
            it->second.price = price;
        }
        return it->second;
    }
    template <typename Map> static void erase_in(Map &levels, Price price) {
        const auto it = levels.find(price);
        assert(it != levels.end() && it->second.queue.empty() && "erasing a missing or busy level");
        levels.erase(it);
    }
    template <typename Map, typename Visit> static void walk(const Map &levels, Visit &visit) {
        for (const auto &[price, level] : levels) {
            if (!visit(level)) {
                return;
            }
        }
    }

    std::map<Price, Level, std::greater<>> bids_; // begin() = highest bid = best
    std::map<Price, Level, std::less<>> asks_;    // begin() = lowest ask = best
};

} // namespace nanobook::fast

/*
 * sorted_levels.hpp: the fast book's price levels: a sorted array per side, best price at the end.
 *
 * What it does: Keeps only the prices that have orders, for both sides, and answers what the book
 *   needs: the best level, the level at a price, a level to add an order to, removing an empty
 *   level, and walking a side best price first.
 *
 * Why it exists: The main price-level design (fast-book design §6.1). Memory grows with the number
 *   of levels in use, not with the price band, so any band works, from a $5 stock to BRK.A. All
 *   levels sit side by side in one array, unlike a std::map that scatters a node per level.
 *
 * How it works: Each side's array is sorted from the worst price to the best, so the best level is
 *   the last element: best() is O(1), and adding or removing near the best price only shifts the
 *   few elements after it (like adding a book at the end of a shelf). Finding a price is a binary
 *   search: look at the middle, keep the half the price must be in, repeat (O(log n): about 7 looks
 *   for 100 levels), after first checking the kNearBest = 4 levels nearest the best one by one.
 *   That hybrid was dropped in phase 2 (slower on evenly spread synthetic orders, design §6.1),
 *   then re-measured on the real Nasdaq day, where orders cluster at the best and books are
 *   thousands of levels deep: 10% more throughput (optimise design E3). Cost: up to 4 compares,
 *   then O(log n).
 *
 * Data structures: Two std::vector<Level> (32 bytes per level), each reserved for max_orders levels
 *   at construction, so they never grow while trading: there can't be more levels on a side than
 *   resting orders. Their PrefaultAllocator touches every page of the reservation then, so no
 *   page is first touched while trading (optimise design E2).
 *
 * Trade-offs: Inserting or removing a level deep in the book shifts every better level along by
 *   one, O(n). That's rare (activity is near the best price), but in a deep book one such insert
 *   costs microseconds: a tail-latency risk for phase 3 to measure (design §6.1). Memory is
 *   2 x max_orders x 32 bytes (64 MB for 1M orders) whether or not it is used. A Level* stays
 *   valid only until the next get_or_create() or erase() on that side, because those shift
 *   elements.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/level.hpp"
#include "nanobook/fast/prefault_allocator.hpp"

namespace nanobook::fast {

class SortedLevels {
    // PrefaultAllocator: every page of the reservation is touched at construction, so no page is
    // first touched mid-replay (optimise design E2).
    using Array = std::vector<Level, PrefaultAllocator<Level>>;

public:
    explicit SortedLevels(const BookConfig &config) {
        bids_.reserve(config.max_orders);
        asks_.reserve(config.max_orders);
    }

    // The level at `price`, or nullptr if there is none.
    [[nodiscard]] Level *find(Side side, Price price) {
        Array &v = levels(side);
        const std::size_t i = lower_bound(side, v, price);
        return i < v.size() && v[i].price == price ? &v[i] : nullptr;
    }
    [[nodiscard]] const Level *find(Side side, Price price) const {
        const Array &v = levels(side);
        const std::size_t i = lower_bound(side, v, price);
        return i < v.size() && v[i].price == price ? &v[i] : nullptr;
    }

    // The best level of a side (highest bid, lowest ask), or nullptr if the side is empty.
    [[nodiscard]] Level *best(Side side) {
        Array &v = levels(side);
        return v.empty() ? nullptr : &v.back();
    }
    [[nodiscard]] const Level *best(Side side) const {
        const Array &v = levels(side);
        return v.empty() ? nullptr : &v.back();
    }

    // The level at `price`, inserted empty at its sorted position if it doesn't exist yet.
    [[nodiscard]] Level &get_or_create(Side side, Price price) {
        Array &v = levels(side);
        const std::size_t i = lower_bound(side, v, price);
        if (i < v.size() && v[i].price == price) {
            return v[i];
        }
        assert(v.size() < v.capacity() && "more levels than max_orders: would reallocate");
        return *v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), Level{.price = price});
    }

    // Removes the level at `price`. It must exist and be empty.
    void erase(Side side, Price price) {
        Array &v = levels(side);
        const std::size_t i = lower_bound(side, v, price);
        assert(i < v.size() && v[i].price == price && v[i].queue.empty() &&
               "erasing a missing or busy level");
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // How many levels are strictly better than `price`: exactly the levels that inserting or
    // erasing a level at `price` shifts along the array. For analysis (ITCH design step 6), not
    // the hot path.
    [[nodiscard]] std::size_t better_than(Side side, Price price) const {
        const Array &v = levels(side);
        std::size_t i = lower_bound(side, v, price); // the first level not worse than `price`
        if (i < v.size() && v[i].price == price) {
            ++i; // the level at `price` itself isn't better
        }
        return v.size() - i;
    }

    // Calls visit(level) for each level of a side, best price first, until visit returns false.
    template <typename Visit> void for_each_best_first(Side side, Visit &&visit) const {
        const Array &v = levels(side);
        for (const Level &level : std::views::reverse(v)) { // the best level is at the end
            if (!visit(level)) {
                return;
            }
        }
    }

private:
    // Is price `a` worse than price `b` on this side? A lower bid, or a higher ask.
    static bool worse(Side side, Price a, Price b) {
        return side == Side::Buy ? a < b : a > b;
    }

    // Check the kNearBest levels nearest the best one by one before the binary search: real
    // orders land near the best price. 4 won on the real day (optimise design E3: 10% more
    // throughput; 8 and 16 were no faster and slowed adds). 0 would be binary search only.
    static constexpr std::size_t kNearBest = 4;

    // The first index whose level is not worse than `price`: where `price` is, or would be
    // inserted. Binary search: the array is sorted worst to best, so everything before that index
    // is worse than `price`, and everything from it on is not.
    static std::size_t lower_bound(Side side, const Array &v, Price price) {
        std::size_t lo = 0;
        std::size_t hi = v.size(); // the answer is always in [lo, hi]
        if constexpr (kNearBest > 0) {
            // From the best end down: the first level found worse than `price` has the answer
            // just after it. If none of the kNearBest is worse, the answer is at or before them.
            const std::size_t stop = hi > kNearBest ? hi - kNearBest : 0;
            for (std::size_t j = hi; j > stop; --j) {
                if (worse(side, v[j - 1].price, price)) {
                    return j;
                }
            }
            hi = stop;
        }
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (worse(side, v[mid].price, price)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    [[nodiscard]] Array &levels(Side side) {
        return side == Side::Buy ? bids_ : asks_;
    }
    [[nodiscard]] const Array &levels(Side side) const {
        return side == Side::Buy ? bids_ : asks_;
    }

    Array bids_; // ascending price: the highest bid (best) is last
    Array asks_; // descending price: the lowest ask (best) is last
};

} // namespace nanobook::fast

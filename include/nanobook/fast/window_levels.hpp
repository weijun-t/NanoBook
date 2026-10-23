/*
 * window_levels.hpp: the fast book's price levels as a moving window of tick slots near the best,
 *   with a sorted array for every other price (E7).
 *
 * What it does: The same interface as SortedLevels. Per side, the W prices around the best each
 *   have a slot, so finding or creating a level there needs no search and shifts nothing. Prices
 *   outside the window (stub quotes) or off its step (sub-penny prices) live in the overflow, a
 *   SortedLevels. When the best leaves the middle half of the window, the window slides to
 *   centre on it.
 *
 * Why it exists: Optimise design E7 (§10). A tick array can't cover a real day's prices, $0.0001
 *   to $200,000, but nearly all activity is near the best: on the real day, 98.3% of level
 *   lookups land in a window of W = 1,024 (§10.5). The levels were about 23% of the replay.
 *
 * How it works: A price's home is decided by arithmetic alone. It is in the window if it is in
 *   [base, base + W x step) and on the step; otherwise it is in the overflow. The step is 1 cent
 *   (100 ticks) when the window is centred on $1 or more, else $0.0001 (SEC Rule 612). The slot
 *   is (price / step) mod W, like hours on a clock face: when the window slides, a price that
 *   stays in it keeps its slot, so a slide only moves the levels that enter or leave. Walking
 *   the window in price order starts at the base's slot and wraps round. The best is the better
 *   of the window's (bitmap scan) and the overflow's (its last element). A slide happens when a
 *   new level would be a best beyond the middle half, or erasing the best leaves the new best
 *   outside it, or a side's first level arrives. Cost: find, create and erase in the window
 *   O(1); best O(W / 4,096) summary words; a slide O(W / 64 + levels in the window + levels
 *   moved); the overflow as SortedLevels.
 *
 * Data structures: Per side: W Levels (32 bytes each) in a std::pmr::vector, a TickBitmap of
 *   which slots are in use, the window's base, step and middle half, and the side's level count.
 *   One SortedLevels for both sides' overflow, reserved for max_orders levels a side as today.
 *   All allocated at construction from the default memory resource (the huge-page arena in the
 *   timed tools, optimise design E6).
 *
 * Trade-offs: Memory: 2 x W x 32 bytes per book on top of SortedLevels (64 KB at W = 1,024). A
 *   slide is a pause of up to a few hundred levels moved (234 on the real day). A Level* stays
 *   valid only until the next get_or_create() or erase() on that side, as for SortedLevels.
 */

#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/level.hpp"
#include "nanobook/fast/sorted_levels.hpp"
#include "nanobook/fast/tick_bitmap.hpp"

namespace nanobook::fast {

template <std::size_t W> class WindowLevels {
    static_assert(W >= 8 && std::has_single_bit(W), "W: a power of two, so mod W is a mask");

public:
    explicit WindowLevels(const BookConfig &config) : overflow_(config) {
    }

    // The bytes both sides' windows and the overflow take from their memory resource, with 64
    // bytes of alignment for each window array (slots, bits, summary: 3 a side).
    [[nodiscard]] static std::size_t arena_bytes(const BookConfig &config) {
        return SortedLevels::arena_bytes(config) +
               2 * (W * sizeof(Level) + TickBitmap::arena_bytes(W) + std::size_t{3} * 64);
    }

    // The level at `price`, or nullptr if there is none.
    [[nodiscard]] Level *find(Side side, Price price) {
        SideLevels &s = sides_[pick(side)];
        if (in_window(s, price.ticks)) {
            const std::size_t i = slot(s, price.ticks);
            return s.used.test(i) ? &s.slots[i] : nullptr;
        }
        return overflow_.find(side, price);
    }
    [[nodiscard]] const Level *find(Side side, Price price) const {
        const SideLevels &s = sides_[pick(side)];
        if (in_window(s, price.ticks)) {
            const std::size_t i = slot(s, price.ticks);
            return s.used.test(i) ? &s.slots[i] : nullptr;
        }
        return overflow_.find(side, price);
    }

    // The best level of a side (highest bid, lowest ask), or nullptr if the side is empty.
    [[nodiscard]] Level *best(Side side) {
        return best_of(*this, side);
    }
    [[nodiscard]] const Level *best(Side side) const {
        return best_of(*this, side);
    }

    // The level at `price`, created empty if it doesn't exist yet.
    [[nodiscard]] Level &get_or_create(Side side, Price price) {
        SideLevels &s = sides_[pick(side)];
        const std::int64_t p = price.ticks;
        if (in_window(s, p)) {
            const std::size_t i = slot(s, p);
            if (s.used.test(i)) {
                return s.slots[i];
            }
        } else if (Level *level = overflow_.find(side, price)) {
            return *level;
        }
        // A new level. If it is a new best beyond the middle half, or the side's only level,
        // the window centres on it first.
        if (s.count == 0 || beyond_middle(side, s, p)) {
            slide(side, s, p);
        }
        ++s.count;
        if (in_window(s, p)) {
            const std::size_t i = slot(s, p);
            s.used.set(i);
            s.slots[i] = Level{.price = price};
            return s.slots[i];
        }
        return overflow_.get_or_create(side, price);
    }

    // Removes the level at `price`. It must exist and be empty.
    void erase(Side side, Price price) {
        SideLevels &s = sides_[pick(side)];
        const std::int64_t p = price.ticks;
        if (in_window(s, p)) {
            const std::size_t i = slot(s, p);
            assert(s.used.test(i) && s.slots[i].queue.empty() && "erasing a missing or busy level");
            s.used.clear(i);
        } else {
            overflow_.erase(side, price);
        }
        --s.count;
        // Only a level at or beyond the middle half's worse edge can have been the best. If it
        // was, and the new best is outside the middle half, the window follows it.
        if (s.count > 0 && !short_of_middle(side, s, p)) {
            const std::int64_t best = best_of(*this, side)->price.ticks;
            if (best < s.low_middle || best >= s.high_middle) {
                slide(side, s, best);
            }
        }
    }

    // Calls visit(level) for each level of a side, best price first, until visit returns false:
    // the window's levels and the overflow's, merged like two sorted lists.
    template <typename Visit> void for_each_best_first(Side side, Visit &&visit) const {
        const std::size_t n = overflow_.size(side);
        std::size_t k = 0; // the next overflow level, counted from the best
        bool going = walk_window(side, sides_[pick(side)], [&](const Level &level) {
            while (k < n && better(side, overflow_.from_best(side, k).price, level.price)) {
                if (!visit(overflow_.from_best(side, k++))) {
                    return false;
                }
            }
            return static_cast<bool>(visit(level));
        });
        while (going && k < n) {
            going = visit(overflow_.from_best(side, k++));
        }
    }

    // For tests: is `price` one of the window's prices on this side right now?
    [[nodiscard]] bool in_window(Side side, Price price) const {
        return in_window(sides_[pick(side)], price.ticks);
    }

private:
    static constexpr std::int64_t kW = static_cast<std::int64_t>(W);
    static constexpr std::int64_t kOneDollar = 10'000; // prices are in $0.0001
    static constexpr std::int64_t kCent = 100;
    static constexpr std::size_t kNpos = TickBitmap::kNpos;

    struct SideLevels {
        std::pmr::vector<Level> slots = std::pmr::vector<Level>(W);
        TickBitmap used{W}; // which slots hold a level
        // Before the first level: a window of negative prices, which no price can be in.
        std::int64_t base{-2 * kW};       // the window's lowest price, a multiple of step
        std::int64_t step{1};             // ticks between neighbouring slots: 1 or kCent
        std::int64_t low_middle{-2 * kW}; // the middle half: [low_middle, high_middle)
        std::int64_t high_middle{-2 * kW};
        std::size_t count{}; // this side's levels, window and overflow
    };

    [[nodiscard]] static std::size_t pick(Side side) {
        return side == Side::Buy ? 0 : 1;
    }

    // Is price `a` better than price `b` on this side? A higher bid, or a lower ask.
    [[nodiscard]] static bool better(Side side, Price a, Price b) {
        return side == Side::Buy ? a > b : a < b;
    }

    // In [base, base + W x step) and on the step (base is a multiple of it).
    [[nodiscard]] static bool in_window(const SideLevels &s, std::int64_t p) {
        return p >= s.base && p < s.base + kW * s.step && (s.step == 1 || p % kCent == 0);
    }

    // (p / step) mod W: a price keeps its slot when the window slides. Dividing by the constant
    // kCent compiles to a multiply; mod W is a mask. Two's complement keeps a negative base's
    // slot consistent with the positive prices after it.
    [[nodiscard]] static std::size_t slot(const SideLevels &s, std::int64_t p) {
        const std::int64_t n = s.step == 1 ? p : p / kCent;
        return static_cast<std::size_t>(n) & (W - 1);
    }

    // A new level at p is a best beyond the middle half: above it for bids, below it for asks.
    // (The current best is always inside the middle half, so p is better than it.)
    [[nodiscard]] static bool beyond_middle(Side side, const SideLevels &s, std::int64_t p) {
        return side == Side::Buy ? p >= s.high_middle : p < s.low_middle;
    }
    // p is worse than every price in the middle half, so it can't be the best.
    [[nodiscard]] static bool short_of_middle(Side side, const SideLevels &s, std::int64_t p) {
        return side == Side::Buy ? p < s.low_middle : p >= s.high_middle;
    }

    // The better of the window's best and the overflow's. Self is WindowLevels or const
    // WindowLevels, so one body serves both best() overloads.
    template <typename Self> static auto *best_of(Self &self, Side side) {
        decltype(self.overflow_.best(side)) window_best = nullptr;
        walk_window(side, self.sides_[pick(side)], [&](auto &level) {
            window_best = &level;
            return false; // the first is the best
        });
        auto *overflow_best = self.overflow_.best(side);
        if (window_best == nullptr ||
            (overflow_best != nullptr && better(side, overflow_best->price, window_best->price))) {
            return overflow_best;
        }
        return window_best;
    }

    // Visits the window's levels best first; false if visit asked to stop. In price order the
    // slots run from the base's slot b up to W - 1, then wrap round from 0 to b - 1.
    template <typename Side_, typename Visit>
    static bool walk_window(Side side, Side_ &s, Visit &&visit) {
        const std::size_t b = slot(s, s.base);
        if (side == Side::Buy) { // highest first: b - 1 down to 0, then W - 1 down to b
            return (b == 0 || walk_down(s, Run{.from = b - 1, .to = 0}, visit)) &&
                   walk_down(s, Run{.from = W - 1, .to = b}, visit);
        }
        return walk_up(s, Run{.from = b, .to = W - 1}, visit) &&
               (b == 0 || walk_up(s, Run{.from = 0, .to = b - 1}, visit));
    }
    // Slots `from` to `to`, both included, in the order they are walked.
    struct Run {
        std::size_t from;
        std::size_t to;
    };
    // Visits the slots in use from run.from down to run.to; false if visit asked to stop.
    template <typename Side_, typename Visit>
    static bool walk_down(Side_ &s, Run run, Visit &visit) {
        for (std::size_t i = s.used.next_down(run.from); i != kNpos && i >= run.to;
             i = i == 0 ? kNpos : s.used.next_down(i - 1)) {
            if (!visit(s.slots[i])) {
                return false;
            }
        }
        return true;
    }
    // Visits the slots in use from run.from up to run.to; false if visit asked to stop.
    template <typename Side_, typename Visit> static bool walk_up(Side_ &s, Run run, Visit &visit) {
        for (std::size_t i = s.used.next_up(run.from); i != kNpos && i <= run.to;
             i = s.used.next_up(i + 1)) {
            if (!visit(s.slots[i])) {
                return false;
            }
        }
        return true;
    }

    // Centres the window on price c: c's slot (rounded down to the step) becomes slot W/2 of
    // the window. Window levels that leave go to the overflow; overflow levels that land on a
    // slot come in. Levels that stay keep their slot, unless the step changes (crossing $1):
    // then every window level goes out first, since its slot number changes.
    void slide(Side side, SideLevels &s, std::int64_t c) {
        const std::int64_t step = c >= kOneDollar ? kCent : 1;
        const std::int64_t base = (c / step) * step - (kW / 2) * step;
        const bool restep = step != s.step;
        for (std::size_t i = s.used.next_up(0); i != kNpos; i = s.used.next_up(i + 1)) {
            const std::int64_t p = s.slots[i].price.ticks;
            if (restep || p < base || p >= base + kW * step) {
                overflow_.get_or_create(side, s.slots[i].price) = s.slots[i]; // its orders too
                s.used.clear(i);
            }
        }
        s.base = base;
        s.step = step;
        s.low_middle = base + (kW / 4) * step;
        s.high_middle = base + (3 * kW / 4) * step;
        overflow_.take(side, Price{base}, Price{base + kW * step - 1}, [&](const Level &level) {
            if (!in_window(s, level.price.ticks)) {
                return false; // off the step: stays in the overflow
            }
            const std::size_t i = slot(s, level.price.ticks);
            assert(!s.used.test(i) && "two prices on one slot");
            s.slots[i] = level;
            s.used.set(i);
            return true;
        });
    }

    std::array<SideLevels, 2> sides_; // [0] = bids, [1] = asks
    SortedLevels overflow_;           // far and off-step prices, both sides
};

} // namespace nanobook::fast

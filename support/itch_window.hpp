/*
 * itch_window.hpp: simulates E7's moving window on prices alone, to choose its width (step 8a').
 *
 * What it does: WindowModel follows, for every book and side, which prices have a level and where
 *   a window of W slots would sit (optimise design §10.4), without building any Level. It counts:
 *   - each level lookup by price, and whether it would land in the window, land in its range but
 *     off the price step, or land outside;
 *   - each slide, and how many levels it would move between the window and the overflow: the
 *     pause a slide would cause.
 *
 * Why it exists: The window's width decides its memory (32 bytes per slot, 2 sides, 8,713 books)
 *   and how often it slides. The design fixed the rule before the data: the smallest W that puts
 *   at least 95% of lookups in the window, within 1 GB (§10.5). This model gets those numbers
 *   from the real day before any of the real structure is written.
 *
 * How it works: Driven by FlowAnalysis (itch_flow.hpp): lookup() for every add, execute, cancel,
 *   delete and replace; created() and removed() when a level appears or vanishes. A side's window
 *   is placed around its first level, with its best in slot W/2. The step is 100 ticks (1 cent)
 *   if the best is $1 or more, else 1 tick ($0.0001), and the window starts on a multiple of it.
 *   Whenever the best leaves the middle half (slots W/4 to 3W/4), the window re-centres on it: a
 *   slide. A slide's cost is the number of levels whose home changes, counted by walking the
 *   side's levels in the old and the new range. Cost: lookup O(1); created and removed
 *   O(log levels) for the set, plus O(levels in both ranges) when they slide.
 *
 * Data structures: per book and side, a std::set of the prices that have a level (to find the
 *   best and walk a range in order), and the window's base and step.
 *
 * Trade-offs: Analysis code: it allocates (std::set nodes), so it never runs in a timed replay.
 *   It counts the first lookup on a side as outside the window, since the window is only placed
 *   when the level is created; that's one lookup per side per book, a negligible undercount.
 */

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "nanobook/core/types.hpp"

namespace nanobook::itch {

inline constexpr std::size_t kMovedBuckets = 25; // up to 2^24 levels moved by one slide

struct WindowStats {
    std::uint64_t lookups{};    // every level looked up by price
    std::uint64_t in_window{};  // ...that would land in a window slot
    std::uint64_t off_step{};   // ...inside the window's range, but not on its step
    std::uint64_t placements{}; // windows placed on a side's first level
    std::uint64_t slides{};
    std::uint64_t moved{};     // levels moved by all slides
    std::uint64_t max_moved{}; // by the largest slide
    // Slides by levels moved: 0, 1, 2-3, 4-7, ...: bucket b > 0 holds [2^(b-1), 2^b).
    std::array<std::uint64_t, kMovedBuckets> moved_per_slide{};
};

// Where a price would live right now.
enum class Home : std::uint8_t { Window, OffStep, Overflow };

class WindowModel {
public:
    static constexpr std::int64_t kOneDollar = 10'000; // ITCH prices: 1 tick = $0.0001
    static constexpr std::int64_t kOneCent = 100;

    WindowModel(std::int64_t width, std::size_t books) : width_(width), sides_(2 * books) {
    }

    [[nodiscard]] std::int64_t width() const {
        return width_;
    }
    [[nodiscard]] const WindowStats &stats() const {
        return stats_;
    }

    // A level looked up by price: an add (get_or_create) or an execute, cancel, delete or
    // replace (find).
    void lookup(std::size_t book, Side side, Price price) {
        ++stats_.lookups;
        const Home home = home_of(state(book, side), price.ticks);
        stats_.in_window += home == Home::Window ? 1 : 0;
        stats_.off_step += home == Home::OffStep ? 1 : 0;
    }

    // A new level at `price`: the window is placed, or slides first if the new level becomes a
    // best outside the middle half (as the real structure would before inserting it).
    void created(std::size_t book, Side side, Price price) {
        SideState &s = state(book, side);
        if (s.step == 0) {
            centre(price.ticks, s.base, s.step);
            ++stats_.placements;
        } else {
            const std::int64_t best =
                s.levels.empty() ? price.ticks : better(side, price.ticks, best_of(side, s));
            if (!in_middle(s, best)) {
                slide(s, best);
            }
        }
        s.levels.insert(price.ticks);
    }

    // The level at `price` vanished: if the side's new best is outside the middle half, slide.
    void removed(std::size_t book, Side side, Price price) {
        SideState &s = state(book, side);
        s.levels.erase(price.ticks);
        if (!s.levels.empty() && !in_middle(s, best_of(side, s))) {
            slide(s, best_of(side, s));
        }
    }

    // For tests: where `price` would live now.
    [[nodiscard]] Home home(std::size_t book, Side side, Price price) const {
        return home_of(sides_[index(book, side)], price.ticks);
    }

private:
    struct SideState {
        std::set<std::int64_t> levels; // prices with a level on this side
        std::int64_t base{};           // the price of slot 0, a multiple of step
        std::int64_t step{};           // ticks per slot; 0 until the window is placed
    };

    [[nodiscard]] static std::size_t index(std::size_t book, Side side) {
        return 2 * book + (side == Side::Buy ? 0 : 1);
    }
    [[nodiscard]] SideState &state(std::size_t book, Side side) {
        return sides_[index(book, side)];
    }

    // The better of two prices: the higher bid, or the lower ask.
    [[nodiscard]] static std::int64_t better(Side side, std::int64_t a, std::int64_t b) {
        return side == Side::Buy ? std::max(a, b) : std::min(a, b);
    }
    [[nodiscard]] static std::int64_t best_of(Side side, const SideState &s) {
        return side == Side::Buy ? *s.levels.rbegin() : *s.levels.begin();
    }

    [[nodiscard]] Home home_of(const SideState &s, std::int64_t p) const {
        return home_in(s.base, s.step, p);
    }
    // A slot if p is in [base, base + W x step) and on the step; OffStep if in that range but
    // between two slots; else the overflow. An unplaced window (step 0) holds nothing.
    [[nodiscard]] Home home_in(std::int64_t base, std::int64_t step, std::int64_t p) const {
        if (step == 0 || p < base || p >= base + width_ * step) {
            return Home::Overflow;
        }
        return (p - base) % step == 0 ? Home::Window : Home::OffStep;
    }

    // Is `best` in the middle half, slots W/4 to 3W/4 (the top one excluded)?
    [[nodiscard]] bool in_middle(const SideState &s, std::int64_t best) const {
        return best >= s.base + (width_ / 4) * s.step && best < s.base + (3 * width_ / 4) * s.step;
    }

    // The window centred on `best`: best's slot (rounded down to the step) is W/2.
    void centre(std::int64_t best, std::int64_t &base, std::int64_t &step) const {
        step = best >= kOneDollar ? kOneCent : 1;
        base = (best / step) * step - (width_ / 2) * step;
    }

    // Re-centre on `best`, counting the levels whose home changes: those in a slot of the old
    // window but not of the new one (to the overflow), and the other way round.
    void slide(SideState &s, std::int64_t best) {
        std::int64_t base = 0;
        std::int64_t step = 0;
        centre(best, base, step);
        std::uint64_t moved = 0;
        const auto count_leaving = [&](std::int64_t from_base, std::int64_t from_step,
                                       std::int64_t to_base, std::int64_t to_step) {
            const auto first = s.levels.lower_bound(from_base);
            const auto last = s.levels.lower_bound(from_base + width_ * from_step);
            for (auto it = first; it != last; ++it) {
                if (home_in(from_base, from_step, *it) == Home::Window &&
                    home_in(to_base, to_step, *it) != Home::Window) {
                    ++moved;
                }
            }
        };
        count_leaving(s.base, s.step, base, step); // window -> overflow
        count_leaving(base, step, s.base, s.step); // overflow -> window
        s.base = base;
        s.step = step;
        ++stats_.slides;
        stats_.moved += moved;
        stats_.max_moved = std::max(stats_.max_moved, moved);
        ++stats_.moved_per_slide[std::min(static_cast<std::size_t>(std::bit_width(moved)),
                                          kMovedBuckets - 1)];
    }

    std::int64_t width_;
    std::vector<SideState> sides_; // [2 x book] = bids, [2 x book + 1] = asks
    WindowStats stats_;
};

} // namespace nanobook::itch

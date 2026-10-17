/*
 * tick_levels.hpp: the fast book's price levels as a tick array: one slot per possible price.
 *
 * What it does: The same interface as SortedLevels, built differently: the level for a price is
 *   simply slot (price - min_price), so finding a level never searches. A bitmap marks which prices
 *   have orders, so the best price, and the next one after it, are found 64 prices at a time.
 *
 * Why it exists: An EXPERIMENT (fast-book design §6.2). It measures how much faster a tick array
 *   could be than the sorted list. That number decides whether a moving-window tick array is worth
 *   building in phase 6 (design §6.3). It is never the default FastBook.
 *
 * How it works: Per side: an array of Levels (one per tick in the band), a bitmap with one bit per
 *   tick (set when that level exists), and a summary bitmap with one bit per 64-tick block (set
 *   when the block has any level). Finding the next existing price: look in the current 64-bit
 *   word; if it's empty, use the summary to jump straight to the next non-empty word, then
 *   std::countr_zero (or countl_zero, going down) finds the bit. Asks walk upwards from the lowest
 *   price, bids downwards from the highest.
 *
 * Data structures: Per side: std::vector<Level> (band x 32 bytes), std::vector<uint64_t> bits
 *   (band / 64 words), std::vector<uint64_t> summary (band / 4096 words). All allocated once.
 *
 * Trade-offs: Memory grows with the price BAND, not with the orders: a $100 stock at ±10% is 64 KB
 *   a side, but BRK.A would need about 450 MB a side. So it only accepts bands of at most 65,536
 *   ticks (2 MB a side), checked by assert, and the contract's "any band" rule is why it stays an
 *   experiment. A Level* stays valid until that level is erased.
 */

#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/level.hpp"

namespace nanobook::fast {

class TickLevels {
public:
    static constexpr std::int64_t kMaxBand = 65'536; // ticks: 2 MB of levels per side

    explicit TickLevels(const BookConfig &config)
        : min_price_(config.min_price.ticks),
          band_(static_cast<std::size_t>(config.max_price.ticks - config.min_price.ticks + 1)),
          sides_{SideLevels{band_}, SideLevels{band_}} {
        assert(config.max_price.ticks - config.min_price.ticks + 1 <= kMaxBand &&
               "TickLevels only supports narrow price bands");
    }

    [[nodiscard]] Level *find(Side side, Price price) {
        const std::size_t i = index(price);
        SideLevels &s = sides_[pick(side)];
        return i < band_ && s.present(i) ? &s.levels[i] : nullptr;
    }
    [[nodiscard]] const Level *find(Side side, Price price) const {
        const std::size_t i = index(price);
        const SideLevels &s = sides_[pick(side)];
        return i < band_ && s.present(i) ? &s.levels[i] : nullptr;
    }

    [[nodiscard]] Level *best(Side side) {
        const std::size_t i = best_index(side);
        return i == kNpos ? nullptr : &sides_[pick(side)].levels[i];
    }
    [[nodiscard]] const Level *best(Side side) const {
        const std::size_t i = best_index(side);
        return i == kNpos ? nullptr : &sides_[pick(side)].levels[i];
    }

    [[nodiscard]] Level &get_or_create(Side side, Price price) {
        const std::size_t i = index(price);
        assert(i < band_ && "price outside the band");
        SideLevels &s = sides_[pick(side)];
        if (!s.present(i)) {
            s.levels[i] = Level{.price = price};
            s.set(i);
        }
        return s.levels[i];
    }

    void erase(Side side, Price price) {
        const std::size_t i = index(price);
        SideLevels &s = sides_[pick(side)];
        assert(i < band_ && s.present(i) && s.levels[i].queue.empty() &&
               "erasing a missing or busy level");
        s.levels[i] = Level{};
        s.clear(i);
    }

    // Calls visit(level) for each level of a side, best price first, until visit returns false.
    template <typename Visit> void for_each_best_first(Side side, Visit &&visit) const {
        const SideLevels &s = sides_[pick(side)];
        if (side == Side::Sell) { // asks: lowest price first, walking up
            for (std::size_t i = s.next_up(0); i != kNpos; i = s.next_up(i + 1)) {
                if (!visit(s.levels[i])) {
                    return;
                }
            }
        } else { // bids: highest price first, walking down
            for (std::size_t i = s.next_down(band_ - 1); i != kNpos;
                 i = i == 0 ? kNpos : s.next_down(i - 1)) {
                if (!visit(s.levels[i])) {
                    return;
                }
            }
        }
    }

private:
    static constexpr std::size_t kNpos = std::numeric_limits<std::size_t>::max();

    // One side: the levels, a bit per tick, and a bit per 64-tick block.
    struct SideLevels {
        explicit SideLevels(std::size_t band)
            : levels(band), bits((band + 63) / 64), summary((bits.size() + 63) / 64) {
        }

        [[nodiscard]] bool present(std::size_t i) const {
            return ((bits[i / 64] >> (i % 64)) & 1U) != 0;
        }
        void set(std::size_t i) {
            bits[i / 64] |= std::uint64_t{1} << (i % 64);
            summary[i / 4096] |= std::uint64_t{1} << ((i / 64) % 64);
        }
        void clear(std::size_t i) {
            const std::size_t w = i / 64;
            bits[w] &= ~(std::uint64_t{1} << (i % 64));
            if (bits[w] == 0) { // the whole 64-tick block is empty now
                summary[w / 64] &= ~(std::uint64_t{1} << (w % 64));
            }
        }

        // The lowest existing level at index >= from, or kNpos.
        [[nodiscard]] std::size_t next_up(std::size_t from) const {
            if (from >= levels.size()) {
                return kNpos;
            }
            const std::size_t w = from / 64;
            if (const std::uint64_t here = bits[w] & (~std::uint64_t{0} << (from % 64))) {
                return w * 64 + static_cast<std::size_t>(std::countr_zero(here));
            }
            // Jump over empty 64-tick blocks with the summary.
            for (std::size_t s = (w + 1) / 64; s < summary.size(); ++s) {
                std::uint64_t blocks = summary[s];
                if (s == (w + 1) / 64) {
                    blocks &= ~std::uint64_t{0} << ((w + 1) % 64); // only blocks after w
                }
                if (blocks != 0) {
                    const std::size_t word =
                        s * 64 + static_cast<std::size_t>(std::countr_zero(blocks));
                    return word * 64 + static_cast<std::size_t>(std::countr_zero(bits[word]));
                }
            }
            return kNpos;
        }

        // The highest existing level at index <= from, or kNpos.
        [[nodiscard]] std::size_t next_down(std::size_t from) const {
            const std::size_t w = from / 64;
            if (const std::uint64_t here = bits[w] & low_bits_through(from % 64)) {
                return w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(here));
            }
            if (w == 0) {
                return kNpos;
            }
            const std::size_t before = w - 1; // the last block to consider
            for (std::size_t s = before / 64 + 1; s-- > 0;) {
                std::uint64_t blocks = summary[s];
                if (s == before / 64) {
                    blocks &= low_bits_through(before % 64); // only blocks up to `before`
                }
                if (blocks != 0) {
                    const std::size_t word =
                        s * 64 + 63 - static_cast<std::size_t>(std::countl_zero(blocks));
                    return word * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits[word]));
                }
            }
            return kNpos;
        }

        // A mask with bits 0..n set (n in 0..63).
        [[nodiscard]] static std::uint64_t low_bits_through(std::size_t n) {
            return n == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (n + 1)) - 1;
        }

        std::vector<Level> levels;
        std::vector<std::uint64_t> bits;
        std::vector<std::uint64_t> summary;
    };

    [[nodiscard]] static std::size_t pick(Side side) {
        return side == Side::Buy ? 0 : 1;
    }
    // Slot of a price, or kNpos (which is >= band_) for any price outside the band. Compares
    // before subtracting, so no price, however extreme, can overflow.
    [[nodiscard]] std::size_t index(Price price) const {
        if (price.ticks < min_price_) {
            return kNpos;
        }
        const auto offset =
            static_cast<std::uint64_t>(price.ticks - min_price_); // >= 0, no overflow
        return offset < band_ ? static_cast<std::size_t>(offset) : kNpos;
    }
    [[nodiscard]] std::size_t best_index(Side side) const {
        const SideLevels &s = sides_[pick(side)];
        return side == Side::Sell ? s.next_up(0) : s.next_down(band_ - 1);
    }

    std::int64_t min_price_;
    std::size_t band_;
    std::array<SideLevels, 2> sides_; // [0] = bids, [1] = asks
};

} // namespace nanobook::fast

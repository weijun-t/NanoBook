/*
 * tick_bitmap.hpp: one bit per tick slot, to find the next slot in use 64 slots at a time.
 *
 * What it does: TickBitmap marks which slots of a tick array are in use: set, clear and test a
 *   slot, and find the nearest slot in use at or above a position (next_up) or at or below it
 *   (next_down).
 *
 * Why it exists: Both tick-array level structures need it: TickLevels (a slot per price in the
 *   band) and E7's moving window (a slot per price near the best, optimise design §10.3). One
 *   copy of the trickiest code, the 64-at-a-time scans (§10.9, W5). Moved out of TickLevels
 *   unchanged.
 *
 * How it works: `bits` has one bit per slot; `summary` has one bit per 64-slot word of `bits`,
 *   set when that word is non-zero. next_up looks in the current word first; if nothing is there,
 *   it uses the summary to jump straight to the next non-zero word, then std::countr_zero ("count
 *   the zeros below the lowest set bit") finds the slot. next_down is the mirror image, with
 *   std::countl_zero. Cost: O(1) within a word, else O(slots / 4,096) summary words.
 *
 * Data structures: two std::pmr::vector<uint64_t>: `bits` (slots / 64 words) and `summary`
 *   (slots / 4,096 words), allocated once from the default memory resource. pmr, so that in the
 *   timed tools they come from the huge-page arena with the levels (optimise design E6).
 *
 * Trade-offs: The summary has to be kept up to date on every set and clear (one more write), in
 *   exchange for skipping 4,096 empty slots per summary word in a sparse array.
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <vector>

namespace nanobook::fast {

class TickBitmap {
public:
    static constexpr std::size_t kNpos = std::numeric_limits<std::size_t>::max();

    explicit TickBitmap(std::size_t slots)
        : slots_(slots), bits_((slots + 63) / 64), summary_((bits_.size() + 63) / 64) {
    }

    // The bytes a bitmap of `slots` takes from its memory resource: its two arrays of words.
    [[nodiscard]] static std::size_t arena_bytes(std::size_t slots) {
        const std::size_t words = (slots + 63) / 64;
        return (words + (words + 63) / 64) * sizeof(std::uint64_t);
    }

    [[nodiscard]] bool test(std::size_t i) const {
        return ((bits_[i / 64] >> (i % 64)) & 1U) != 0;
    }
    void set(std::size_t i) {
        bits_[i / 64] |= std::uint64_t{1} << (i % 64);
        summary_[i / 4096] |= std::uint64_t{1} << ((i / 64) % 64);
    }
    void clear(std::size_t i) {
        const std::size_t w = i / 64;
        bits_[w] &= ~(std::uint64_t{1} << (i % 64));
        if (bits_[w] == 0) { // the whole 64-slot block is empty now
            summary_[w / 64] &= ~(std::uint64_t{1} << (w % 64));
        }
    }

    // The lowest slot in use at index >= from, or kNpos.
    [[nodiscard]] std::size_t next_up(std::size_t from) const {
        if (from >= slots_) {
            return kNpos;
        }
        const std::size_t w = from / 64;
        if (const std::uint64_t here = bits_[w] & (~std::uint64_t{0} << (from % 64))) {
            return w * 64 + static_cast<std::size_t>(std::countr_zero(here));
        }
        // Jump over empty 64-slot blocks with the summary.
        for (std::size_t s = (w + 1) / 64; s < summary_.size(); ++s) {
            std::uint64_t blocks = summary_[s];
            if (s == (w + 1) / 64) {
                blocks &= ~std::uint64_t{0} << ((w + 1) % 64); // only blocks after w
            }
            if (blocks != 0) {
                const std::size_t word =
                    s * 64 + static_cast<std::size_t>(std::countr_zero(blocks));
                return word * 64 + static_cast<std::size_t>(std::countr_zero(bits_[word]));
            }
        }
        return kNpos;
    }

    // The highest slot in use at index <= from, or kNpos.
    [[nodiscard]] std::size_t next_down(std::size_t from) const {
        const std::size_t w = from / 64;
        if (const std::uint64_t here = bits_[w] & low_bits_through(from % 64)) {
            return w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(here));
        }
        if (w == 0) {
            return kNpos;
        }
        const std::size_t before = w - 1; // the last block to consider
        for (std::size_t s = before / 64 + 1; s-- > 0;) {
            std::uint64_t blocks = summary_[s];
            if (s == before / 64) {
                blocks &= low_bits_through(before % 64); // only blocks up to `before`
            }
            if (blocks != 0) {
                const std::size_t word =
                    s * 64 + 63 - static_cast<std::size_t>(std::countl_zero(blocks));
                return word * 64 + 63 - static_cast<std::size_t>(std::countl_zero(bits_[word]));
            }
        }
        return kNpos;
    }

private:
    // A mask with bits 0..n set (n in 0..63).
    [[nodiscard]] static std::uint64_t low_bits_through(std::size_t n) {
        return n == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (n + 1)) - 1;
    }

    std::size_t slots_;
    std::pmr::vector<std::uint64_t> bits_;
    std::pmr::vector<std::uint64_t> summary_;
};

} // namespace nanobook::fast

/*
 * test_tick_bitmap.cpp: unit tests for TickBitmap (include/nanobook/fast/tick_bitmap.hpp).
 *
 * What it does: Sets and clears slots by hand and checks next_up and next_down: at the first and
 *   last slot, within one 64-slot word, across empty words and across 4,096-slot summary words,
 *   and after a whole word has been emptied again.
 *
 * Why it exists: Both tick-array level structures find the best price with it. The TickLevels
 *   tests never emptied a whole word and then searched past it, so a summary bit left set by
 *   clear() went unnoticed (only the fuzzer crashed): next_up then jumps to an empty word, and
 *   countr_zero(0) = 64 points one word too far.
 */

#include <gtest/gtest.h>

#include <cstddef>

#include "nanobook/fast/tick_bitmap.hpp"

using nanobook::fast::TickBitmap;

namespace {

constexpr std::size_t kNpos = TickBitmap::kNpos;

} // namespace

TEST(TickBitmap, EmptyFindsNothing) {
    const TickBitmap b{10'000};
    EXPECT_EQ(b.next_up(0), kNpos);
    EXPECT_EQ(b.next_down(9'999), kNpos);
    EXPECT_EQ(b.next_up(10'000), kNpos); // past the end
}

TEST(TickBitmap, FindsTheFirstAndLastSlot) {
    TickBitmap b{10'000};
    b.set(0);
    b.set(9'999);
    EXPECT_TRUE(b.test(0));
    EXPECT_FALSE(b.test(1));
    EXPECT_EQ(b.next_up(0), 0U);
    EXPECT_EQ(b.next_up(1), 9'999U); // across empty words and summary words
    EXPECT_EQ(b.next_down(9'999), 9'999U);
    EXPECT_EQ(b.next_down(9'998), 0U);
}

TEST(TickBitmap, SearchesWithinAWordAndAcrossSummaryWords) {
    TickBitmap b{10'000};
    b.set(70);    // word 1
    b.set(100);   // word 1
    b.set(5'000); // word 78, summary word 1
    EXPECT_EQ(b.next_up(71), 100U);
    EXPECT_EQ(b.next_up(101), 5'000U);
    EXPECT_EQ(b.next_down(4'999), 100U);
    EXPECT_EQ(b.next_down(99), 70U);
    EXPECT_EQ(b.next_down(69), kNpos);
}

// The case the TickLevels tests missed: empty a whole word, then search past it.
TEST(TickBitmap, ClearingAWholeWordClearsItsSummaryBit) {
    TickBitmap b{10'000};
    b.set(130); // word 2
    b.set(140); // word 2
    b.set(300); // word 4
    b.clear(130);
    b.clear(140); // word 2 is empty now
    EXPECT_FALSE(b.test(130));
    EXPECT_EQ(b.next_up(0), 300U);
    EXPECT_EQ(b.next_down(299), kNpos);
    b.clear(300); // nothing left
    EXPECT_EQ(b.next_up(0), kNpos);
    EXPECT_EQ(b.next_down(9'999), kNpos);
}

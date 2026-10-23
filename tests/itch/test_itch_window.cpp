/*
 * test_itch_window.cpp: tests for the moving-window model (support/itch_window.hpp, step 8a').
 *
 * What it does: Drives a WindowModel of 8 slots by hand, with every placement, slide and level
 *   moved worked out in the comments, and checks where prices would live at the window's edges,
 *   off its step and on an unplaced side, and what lookups are counted as.
 *
 * Why it exists: The window's width is chosen from these counts (optimise design §10.5). An edge
 *   off by one slot, or a slide counted wrong, would quietly change that choice.
 */

#include <gtest/gtest.h>

#include "itch_window.hpp"
#include "nanobook/core/types.hpp"

using namespace nanobook;
using namespace nanobook::itch;

namespace {

Price p(std::int64_t ticks) {
    return Price{ticks};
}

} // namespace

// Prices in $0.0001. W = 8, so the middle half is slots 2 to 5.
TEST(WindowModel, PlacesSlidesAndCountsTheLevelsMovedByHand) {
    WindowModel m{8, 2};

    // Bids. The first level, $10.00: step 1 cent (100), its slot 4. Slots 99,600..100,300.
    m.created(0, Side::Buy, p(100'000));
    EXPECT_EQ(m.home(0, Side::Buy, p(99'600)), Home::Window);    // slot 0
    EXPECT_EQ(m.home(0, Side::Buy, p(99'500)), Home::Overflow);  // just below
    EXPECT_EQ(m.home(0, Side::Buy, p(100'300)), Home::Window);   // slot 7, the last
    EXPECT_EQ(m.home(0, Side::Buy, p(100'400)), Home::Overflow); // just above
    EXPECT_EQ(m.home(0, Side::Buy, p(100'050)), Home::OffStep);  // half a cent

    m.created(0, Side::Buy, p(99'600));  // best still 100,000 (slot 4)
    m.created(0, Side::Buy, p(1));       // a stub quote, far below
    m.created(0, Side::Buy, p(100'100)); // the best, slot 5: still in the middle half
    EXPECT_EQ(m.stats().slides, 0U);

    // 100,200 would be the best in slot 6: slide. New slots 99,800..100,500 (best in slot 4).
    // 99,600 leaves the window; 100,000 and 100,100 stay; the stub was never in either.
    m.created(0, Side::Buy, p(100'200));
    EXPECT_EQ(m.stats().slides, 1U);
    EXPECT_EQ(m.stats().moved, 1U);
    EXPECT_EQ(m.home(0, Side::Buy, p(99'600)), Home::Overflow);
    EXPECT_EQ(m.home(0, Side::Buy, p(100'500)), Home::Window);

    // Removing 100,200 then 100,100: the best (100,100, then 100,000) is in slots 3 and 2.
    m.removed(0, Side::Buy, p(100'200));
    m.removed(0, Side::Buy, p(100'100));
    EXPECT_EQ(m.stats().slides, 1U);

    // Removing 100,000: the best is 99,600, below the window: slide to 99,200..99,900, which
    // takes 99,600 back in.
    m.removed(0, Side::Buy, p(100'000));
    EXPECT_EQ(m.stats().slides, 2U);
    EXPECT_EQ(m.stats().moved, 2U);
    EXPECT_EQ(m.stats().max_moved, 1U);
    EXPECT_EQ(m.stats().moved_per_slide[1], 2U); // both slides moved exactly 1 level

    // Asks under $1: step $0.0001. The first level, 5,000: slots 4,996..5,003.
    m.created(0, Side::Sell, p(5'000));
    EXPECT_EQ(m.home(0, Side::Sell, p(5'003)), Home::Window);
    EXPECT_EQ(m.home(0, Side::Sell, p(5'004)), Home::Overflow);
    // 4,997 is a LOWER ask, so the new best, in slot 1: slide to 4,993..5,000. 5,000 stays in.
    m.created(0, Side::Sell, p(4'997));
    EXPECT_EQ(m.stats().slides, 3U);
    EXPECT_EQ(m.stats().moved_per_slide[0], 1U); // that slide moved nothing
    EXPECT_EQ(m.stats().placements, 2U);

    // Book 1 has no levels yet: its window isn't placed, so nothing is in it.
    EXPECT_EQ(m.home(1, Side::Buy, p(100'000)), Home::Overflow);
}

TEST(WindowModel, CountsWhereEachLookupWouldLand) {
    WindowModel m{8, 2};
    m.created(0, Side::Buy, p(100'000)); // slots 99,600..100,300
    m.lookup(0, Side::Buy, p(100'000));  // in the window
    m.lookup(0, Side::Buy, p(99'650));   // off the step
    m.lookup(0, Side::Buy, p(1));        // the overflow
    m.lookup(1, Side::Sell, p(5'000));   // an unplaced side: the overflow
    EXPECT_EQ(m.stats().lookups, 4U);
    EXPECT_EQ(m.stats().in_window, 1U);
    EXPECT_EQ(m.stats().off_step, 1U);
}

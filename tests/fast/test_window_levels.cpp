/*
 * test_window_levels.cpp: unit tests for E7's moving window (fast/window_levels.hpp).
 *
 * What it does: Drives a window of 8 slots by hand, with every window position worked out in the
 *   comments: placing it on a side's first level, its edges, a sub-penny best kept in the
 *   overflow, slides up and down that keep each level's contents, crossing $1 (the step changes),
 *   and an emptied side starting again elsewhere. Then random operations on cent prices, against
 *   std::map, at 8 and 1,024 slots.
 *
 * Why it exists: The new failure modes of optimise design §10.6: a price at an edge, an off-step
 *   price folded into a slot, a best missed in the overflow, a slide that loses a level. The typed
 *   tests in test_levels.cpp use prices under $1 only, so the 1-cent step is tested here.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/level.hpp"
#include "nanobook/fast/window_levels.hpp"

using namespace nanobook;
using namespace nanobook::fast;

namespace {

constexpr BookConfig kConfig{.min_price = Price{1},
                             .max_price = Price{2'000'000'000},
                             .max_orders = 1000,
                             .max_qty = Qty{1'000'000}};

template <typename Levels> std::vector<std::int64_t> walk(const Levels &levels, Side side) {
    std::vector<std::int64_t> prices;
    levels.for_each_best_first(side, [&](const Level &level) {
        prices.push_back(level.price.ticks);
        return true;
    });
    return prices;
}

using Window8 = WindowLevels<8>;

void add(Window8 &w, Side side, std::int64_t price, std::int64_t total = 0) {
    w.get_or_create(side, Price{price}).total = Qty{total};
}

} // namespace

// Prices in $0.0001. W = 8: the middle half is slots 2 to 5, and a slide puts the best in slot 4.

TEST(WindowLevels, PlacesTheWindowOnTheFirstLevel) {
    Window8 w{kConfig};
    add(w, Side::Buy, 100'000);                           // $10: step 1 cent. Slots 99,600..100,300
    EXPECT_TRUE(w.in_window(Side::Buy, Price{99'600}));   // slot 0
    EXPECT_FALSE(w.in_window(Side::Buy, Price{99'500}));  // just below
    EXPECT_TRUE(w.in_window(Side::Buy, Price{100'300}));  // slot 7, the last
    EXPECT_FALSE(w.in_window(Side::Buy, Price{100'400})); // just above
    EXPECT_FALSE(w.in_window(Side::Buy, Price{100'050})); // half a cent: off the step
    EXPECT_FALSE(w.in_window(Side::Sell, Price{100'000})); // the asks have no window yet
}

TEST(WindowLevels, ASubPennyBestLivesInTheOverflowAndIsStillTheBest) {
    Window8 w{kConfig};
    add(w, Side::Buy, 100'000);
    add(w, Side::Buy, 100'050); // better, inside the middle half (below 100,200): no slide
    EXPECT_FALSE(w.in_window(Side::Buy, Price{100'050}));
    EXPECT_EQ(w.best(Side::Buy)->price, Price{100'050}); // found in the overflow
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{100'050, 100'000}));
    w.erase(Side::Buy, Price{100'050});
    EXPECT_EQ(w.best(Side::Buy)->price, Price{100'000});
}

TEST(WindowLevels, SlidesMoveLevelsWithTheirContents) {
    Window8 w{kConfig};
    add(w, Side::Buy, 100'000, 1);
    add(w, Side::Buy, 99'600, 2); // slot 0
    add(w, Side::Buy, 1, 3);      // a stub quote: the overflow
    // 100,200 would be a best in slot 6: slide to 99,800..100,500. 99,600 leaves the window.
    add(w, Side::Buy, 100'200, 4);
    EXPECT_FALSE(w.in_window(Side::Buy, Price{99'600}));
    EXPECT_TRUE(w.in_window(Side::Buy, Price{100'500}));
    EXPECT_EQ(w.find(Side::Buy, Price{99'600})->total, Qty{2}); // moved, not lost
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{100'200, 100'000, 99'600, 1}));

    // The best becomes 100,000 (slot 2, the middle half's edge): no slide.
    w.erase(Side::Buy, Price{100'200});
    EXPECT_TRUE(w.in_window(Side::Buy, Price{100'500}));
    // The best becomes 99,600, below the window: slide to 99,200..99,900, and 99,600 comes back.
    w.erase(Side::Buy, Price{100'000});
    EXPECT_TRUE(w.in_window(Side::Buy, Price{99'600}));
    EXPECT_EQ(w.find(Side::Buy, Price{99'600})->total, Qty{2});
    EXPECT_EQ(w.best(Side::Buy)->price, Price{99'600});
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{99'600, 1}));
}

// E7b: a slot's price says whether it holds a level, so a level that leaves must empty its slot.
TEST(WindowLevels, ASlotLeftBehindByASlideIsEmptyWhenTheWindowReturns) {
    Window8 w{kConfig};
    add(w, Side::Buy, 100'000);
    add(w, Side::Buy, 99'600);         // slot 996 mod 8 = 4
    add(w, Side::Buy, 100'200);        // slide to 99,800..100,500: 99,600 moves to the overflow
    w.erase(Side::Buy, Price{99'600}); // gone from the overflow
    add(w, Side::Buy, 99'300);         // below the window: the overflow
    w.erase(Side::Buy, Price{100'200});
    // The best becomes 99,300: slide to 98,900..99,600, over 99,600's old slot again.
    w.erase(Side::Buy, Price{100'000});
    EXPECT_TRUE(w.in_window(Side::Buy, Price{99'600}));
    EXPECT_EQ(w.find(Side::Buy, Price{99'600}), nullptr);
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{99'300}));
}

TEST(WindowLevels, AsksSlideDownwards) {
    Window8 w{kConfig};
    add(w, Side::Sell, 5'000); // $0.50: step $0.0001. Slots 4,996..5,003
    // 4,997 is a lower ask, a best in slot 1: slide to 4,993..5,000. 5,000 stays, in slot 7.
    add(w, Side::Sell, 4'997);
    EXPECT_TRUE(w.in_window(Side::Sell, Price{4'993}));
    EXPECT_FALSE(w.in_window(Side::Sell, Price{5'001}));
    add(w, Side::Sell, 5'001); // worse than the window: the overflow, no slide
    EXPECT_TRUE(w.in_window(Side::Sell, Price{4'993}));
    EXPECT_EQ(walk(w, Side::Sell), (std::vector<std::int64_t>{4'997, 5'000, 5'001}));
}

TEST(WindowLevels, CrossingADollarChangesTheStep) {
    Window8 w{kConfig};
    add(w, Side::Buy, 9'999); // $0.9999: step $0.0001. Slots 9,995..10,002
    // $1.01 is a best beyond the window: slide, step 1 cent. Slots 9,700..10,400. 9,999 isn't
    // a whole cent, so it moves to the overflow.
    add(w, Side::Buy, 10'100);
    EXPECT_TRUE(w.in_window(Side::Buy, Price{10'100}));
    EXPECT_FALSE(w.in_window(Side::Buy, Price{9'999}));
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{10'100, 9'999}));
    // The best becomes 9,999, inside the middle half (9,900..10,299): no slide.
    w.erase(Side::Buy, Price{10'100});
    EXPECT_EQ(w.best(Side::Buy)->price, Price{9'999});
    add(w, Side::Buy, 10'000); // $1.00 exactly: a slot
    EXPECT_TRUE(w.in_window(Side::Buy, Price{10'000}));
    EXPECT_EQ(walk(w, Side::Buy), (std::vector<std::int64_t>{10'000, 9'999}));
}

TEST(WindowLevels, AnEmptiedSideStartsAgainAroundItsNextLevel) {
    Window8 w{kConfig};
    add(w, Side::Buy, 100'000);
    w.erase(Side::Buy, Price{100'000});
    EXPECT_EQ(w.best(Side::Buy), nullptr);
    add(w, Side::Buy, 5'000); // far from the old window: placed again around it
    EXPECT_TRUE(w.in_window(Side::Buy, Price{5'000}));
    EXPECT_FALSE(w.in_window(Side::Buy, Price{100'000}));
}

// Random creates and erases on both sides around $10, in whole cents, sub-pennies and far away,
// against a std::map per side, compared after every step.
template <typename Levels> void agrees_with_std_map_on_cent_prices() {
    Levels levels{kConfig};
    std::map<std::int64_t, int, std::greater<>> bids; // best (highest) first
    std::map<std::int64_t, int> asks;                 // best (lowest) first
    std::mt19937_64 rng{2026};
    for (int step = 0; step < 20'000; ++step) {
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        const std::uint64_t kind = rng() % 20;
        const std::int64_t mid = side == Side::Buy ? 99'900 : 100'100;
        const auto drift = static_cast<std::int64_t>(rng() % 21) - 10; // -10..10 cents
        std::int64_t price = mid + drift * 100; // whole cents near the middle
        if (kind == 0) {
            price += 50; // a sub-penny price
        } else if (kind == 1) {
            price = 1 + static_cast<std::int64_t>(rng() % 2'000'000); // anywhere, up to $200
        }
        const bool exists = side == Side::Buy ? bids.contains(price) : asks.contains(price);
        if (exists && rng() % 2 == 0) {
            levels.erase(side, Price{price});
            side == Side::Buy ? (void)bids.erase(price) : (void)asks.erase(price);
        } else {
            (void)levels.get_or_create(side, Price{price});
            side == Side::Buy ? (void)bids.emplace(price, 0) : (void)asks.emplace(price, 0);
        }
        std::vector<std::int64_t> want_bids;
        std::vector<std::int64_t> want_asks;
        want_bids.reserve(bids.size());
        want_asks.reserve(asks.size());
        for (const auto &[p, unused] : bids) {
            want_bids.push_back(p);
        }
        for (const auto &[p, unused] : asks) {
            want_asks.push_back(p);
        }
        ASSERT_EQ(walk(levels, Side::Buy), want_bids) << "step " << step;
        ASSERT_EQ(walk(levels, Side::Sell), want_asks) << "step " << step;
        ASSERT_EQ(levels.find(side, Price{price}) != nullptr,
                  side == Side::Buy ? bids.contains(price) : asks.contains(price));
    }
}

TEST(WindowLevels, AgreesWithStdMapOnCentPricesAt8Slots) {
    agrees_with_std_map_on_cent_prices<WindowLevels<8>>();
}

TEST(WindowLevels, AgreesWithStdMapOnCentPricesAt1024Slots) {
    agrees_with_std_map_on_cent_prices<WindowLevels<1024>>();
}

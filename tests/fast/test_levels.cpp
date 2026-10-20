/*
 * test_levels.cpp: unit tests for the fast book's price-level structures.
 *
 * What it does: Runs the same tests on both level structures, SortedLevels and TickLevels. They
 *   cover the best price on each side, inserting at the best end, the deep end and the middle,
 *   finding missing prices, erasing, and walking best-first, then a mini differential test against
 *   std::map on random operations. Extra TickLevels tests cover the band edges and jumps across
 *   empty 64-tick and 4,096-tick blocks.
 *
 * Why it exists: A wrong lookup puts an order on the wrong level or loses a level, which breaks
 *   price priority. Each structure is tested on its own before the book uses it.
 *
 * How it works: Typed tests over the two structures. walk() lists a side's prices best-first, the
 *   order matching uses.
 */

#include <gtest/gtest.h>
#include "nanobook/fast/sorted_levels.hpp"
#include "nanobook/fast/tick_levels.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <vector>

using namespace nanobook;
using namespace nanobook::fast;

namespace {

constexpr BookConfig kConfig{.min_price = Price{1},
                             .max_price = Price{10'000},
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

template <typename Levels> class LevelsTest : public ::testing::Test {
protected:
    Levels levels{kConfig};

    void add(Side side, std::int64_t price) {
        (void)levels.get_or_create(side, Price{price});
    }
};

using LevelTypes = ::testing::Types<SortedLevels, TickLevels>;

struct LevelTypeNames {
    template <typename T> static std::string GetName(int /*index*/) {
        return std::is_same_v<T, SortedLevels> ? "Sorted" : "Tick";
    }
};

} // namespace

TYPED_TEST_SUITE(LevelsTest, LevelTypes, LevelTypeNames);

TYPED_TEST(LevelsTest, EmptySidesHaveNoBest) {
    EXPECT_EQ(this->levels.best(Side::Buy), nullptr);
    EXPECT_EQ(this->levels.best(Side::Sell), nullptr);
    EXPECT_EQ(this->levels.find(Side::Buy, Price{100}), nullptr);
    EXPECT_TRUE(walk(this->levels, Side::Buy).empty());
}

TYPED_TEST(LevelsTest, GetOrCreateReturnsTheSameLevelAgain) {
    Level &first = this->levels.get_or_create(Side::Buy, Price{100});
    first.total = Qty{42};
    const Level &again = this->levels.get_or_create(Side::Buy, Price{100});
    EXPECT_EQ(again.total, Qty{42}); // not a second, new level
    EXPECT_EQ(again.price, Price{100});
    EXPECT_EQ(walk(this->levels, Side::Buy).size(), 1U);
}

TYPED_TEST(LevelsTest, BestIsTheHighestBidAndTheLowestAsk) {
    for (const std::int64_t p : {100, 105, 95, 101}) {
        this->add(Side::Buy, p);
        this->add(Side::Sell, p + 1000);
    }
    EXPECT_EQ(this->levels.best(Side::Buy)->price, Price{105});
    EXPECT_EQ(this->levels.best(Side::Sell)->price, Price{1095});
    EXPECT_EQ(walk(this->levels, Side::Buy), (std::vector<std::int64_t>{105, 101, 100, 95}));
    EXPECT_EQ(walk(this->levels, Side::Sell), (std::vector<std::int64_t>{1095, 1100, 1101, 1105}));
}

TYPED_TEST(LevelsTest, InsertAtTheBestEndTheDeepEndAndTheMiddle) {
    for (const std::int64_t p : {100, 90, 80}) {
        this->add(Side::Buy, p);
    }
    this->add(Side::Buy, 110); // new best
    this->add(Side::Buy, 70);  // new deepest
    this->add(Side::Buy, 85);  // in the middle
    EXPECT_EQ(walk(this->levels, Side::Buy), (std::vector<std::int64_t>{110, 100, 90, 85, 80, 70}));
}

TYPED_TEST(LevelsTest, FindsOnlyPricesThatExist) {
    for (const std::int64_t p : {10, 20, 30}) {
        this->add(Side::Sell, p);
    }
    EXPECT_NE(this->levels.find(Side::Sell, Price{20}), nullptr);
    EXPECT_EQ(this->levels.find(Side::Sell, Price{25}), nullptr); // between two levels
    EXPECT_EQ(this->levels.find(Side::Sell, Price{5}), nullptr);  // better than the best
    EXPECT_EQ(this->levels.find(Side::Sell, Price{35}), nullptr); // worse than the worst
    EXPECT_EQ(this->levels.find(Side::Buy, Price{20}), nullptr);  // other side
    EXPECT_EQ(this->levels.find(Side::Sell, Price{0}), nullptr);  // outside the band
    EXPECT_EQ(this->levels.find(Side::Sell, Price{-5}), nullptr);
    EXPECT_EQ(this->levels.find(Side::Sell, Price{20'000}), nullptr);
}

TYPED_TEST(LevelsTest, EraseTheBestThenTheMiddle) {
    for (const std::int64_t p : {100, 90, 80}) {
        this->add(Side::Buy, p);
    }
    this->levels.erase(Side::Buy, Price{100});
    EXPECT_EQ(this->levels.best(Side::Buy)->price, Price{90}); // the next one takes over
    this->levels.erase(Side::Buy, Price{90});
    EXPECT_EQ(walk(this->levels, Side::Buy), (std::vector<std::int64_t>{80}));
    this->levels.erase(Side::Buy, Price{80});
    EXPECT_EQ(this->levels.best(Side::Buy), nullptr);
}

TYPED_TEST(LevelsTest, WalkStopsWhenVisitReturnsFalse) {
    for (const std::int64_t p : {10, 20, 30, 40}) {
        this->add(Side::Sell, p);
    }
    int visited = 0;
    this->levels.for_each_best_first(Side::Sell, [&](const Level &) { return ++visited < 2; });
    EXPECT_EQ(visited, 2);
}

// The same idea as the differential fuzzer, at small scale: random creates and erases on both
// sides, with a std::map per side as the trusted answer key, compared after every step.
TYPED_TEST(LevelsTest, AgreesWithStdMapOnRandomOperations) {
    std::map<std::int64_t, int, std::greater<>> bids; // best (highest) first
    std::map<std::int64_t, int> asks;                 // best (lowest) first
    std::mt19937_64 rng{777};
    for (int step = 0; step < 20'000; ++step) {
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        // Clustered prices (most activity near the middle) plus some far away.
        const auto price =
            static_cast<std::int64_t>(rng() % 10 == 0 ? 1 + rng() % 10'000 : 4'990 + rng() % 20);
        const bool exists = side == Side::Buy ? bids.contains(price) : asks.contains(price);
        if (exists && rng() % 2 == 0) {
            this->levels.erase(side, Price{price});
            side == Side::Buy ? (void)bids.erase(price) : (void)asks.erase(price);
        } else {
            this->add(side, price);
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
        ASSERT_EQ(walk(this->levels, Side::Buy), want_bids) << "step " << step;
        ASSERT_EQ(walk(this->levels, Side::Sell), want_asks) << "step " << step;
        ASSERT_EQ(this->levels.find(side, Price{price}) != nullptr,
                  side == Side::Buy ? bids.contains(price) : asks.contains(price));
    }
}

// ---------------------------------------------------------------------------
// TickLevels only: the band and the bitmap
// ---------------------------------------------------------------------------

TEST(TickLevels, BandEdgesWork) {
    TickLevels levels{kConfig}; // band [1, 10000]
    (void)levels.get_or_create(Side::Buy, Price{1});
    (void)levels.get_or_create(Side::Buy, Price{10'000});
    EXPECT_EQ(levels.best(Side::Buy)->price, Price{10'000});
    levels.erase(Side::Buy, Price{10'000});
    EXPECT_EQ(levels.best(Side::Buy)->price, Price{1});
}

TEST(TickLevels, JumpsAcrossEmptyBlocks) {
    TickLevels levels{kConfig};
    // 1 and 9,000 are thousands of ticks (several 4,096-tick summary blocks) apart.
    (void)levels.get_or_create(Side::Sell, Price{9'000});
    (void)levels.get_or_create(Side::Sell, Price{1});
    EXPECT_EQ(walk(levels, Side::Sell), (std::vector<std::int64_t>{1, 9'000}));
    levels.erase(Side::Sell, Price{1});
    EXPECT_EQ(levels.best(Side::Sell)->price, Price{9'000}); // found across the empty blocks
    (void)levels.get_or_create(Side::Buy, Price{70});
    (void)levels.get_or_create(Side::Buy, Price{8'191}); // the last tick of a 64-tick word...
    (void)levels.get_or_create(Side::Buy,
                               Price{8'193}); // ...and the first tick after a summary block
    EXPECT_EQ(walk(levels, Side::Buy), (std::vector<std::int64_t>{8'193, 8'191, 70}));
}

TEST(TickLevels, EmptyingTheLastLevelEmptiesTheSide) {
    TickLevels levels{kConfig};
    (void)levels.get_or_create(Side::Sell, Price{500});
    levels.erase(Side::Sell, Price{500});
    EXPECT_EQ(levels.best(Side::Sell), nullptr);
    EXPECT_TRUE(walk(levels, Side::Sell).empty());
}

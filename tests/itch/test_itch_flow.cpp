/*
 * test_itch_flow.cpp: tests for the real-flow analysis (support/itch_flow.hpp, step 6).
 *
 * What it does: First the query it relies on: SortedLevels::better_than() counts the levels
 *   better than a price, on both sides, whether or not that price has a level. Then a small
 *   hand-made day whose every level created or removed, the levels each one shifted, and each
 *   new order's distance from the best price are worked out in the comments, and FlowAnalysis
 *   must count exactly those. Last, the analysis must leave the books as a plain replay does.
 *
 * Why it exists: Step 6's answer to "how often does real flow hit the sorted array's slow case?"
 *   comes from these counts. A count off by one level would quietly change that answer.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "itch_encoder.hpp"
#include "itch_flow.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/fast/sorted_levels.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"

using namespace nanobook;
using namespace nanobook::itch;

namespace {

constexpr Symbol kAaa{'A', 'A', 'A', ' ', ' ', ' ', ' ', ' '};

AddOrder buy(std::uint64_t t, std::uint64_t ref, std::uint32_t shares, std::uint32_t price) {
    return AddOrder{.locate = 1,
                    .timestamp = t,
                    .order_ref = ref,
                    .side = 'B',
                    .shares = shares,
                    .stock = kAaa,
                    .price = price,
                    .attribution = {}};
}

// AAA's bids, prices in $0.0001. "shift" = levels better than the level created or removed.
//   1  ref 1 buys 100 @ 100,000   side empty            new level, shift 0
//   2  ref 2 buys 100 @ 100,100   improves the best     new level, shift 0
//   3  ref 3 buys 100 @  99,000   1,100 behind: < $1    new level, shift 2 (100,100 and 100,000)
//   4  ref 4 buys  50 @ 100,000   100 behind: < 10c     existing level
//   5  ref 5 buys  10 @       1   a stub quote: < $100  new level, shift 3
//   6  delete ref 3               99,000 vanishes, shift 2
//   7  ref 1 fully executed       100,000 keeps ref 4
//   8  ref 4 cancels all 50       100,000 vanishes, shift 1 (100,100)
//   9  ref 2 -> ref 6, 10 @ 100,200: 100,100 vanishes (shift 0); 100,200 improves the best and
//      is a new level, shift 0
std::vector<std::byte> flow_day() {
    Writer w;
    w.stock_directory({.locate = 1, .timestamp = 0, .stock = kAaa});
    w.add_order(buy(1, 1, 100, 100'000));
    w.add_order(buy(2, 2, 100, 100'100));
    w.add_order(buy(3, 3, 100, 99'000));
    w.add_order(buy(4, 4, 50, 100'000));
    w.add_order(buy(5, 5, 10, 1));
    w.delete_order({.locate = 1, .timestamp = 6, .order_ref = 3});
    w.executed(
        {.locate = 1, .timestamp = 7, .order_ref = 1, .executed_shares = 100, .match_number = 1});
    w.cancel({.locate = 1, .timestamp = 8, .order_ref = 4, .cancelled_shares = 50});
    w.replace({.locate = 1,
               .timestamp = 9,
               .original_order_ref = 2,
               .new_order_ref = 6,
               .shares = 10,
               .price = 100'200});
    const std::span<const std::byte> bytes = w.bytes();
    return {bytes.begin(), bytes.end()};
}

std::vector<BookConfig> configs_for(std::span<const std::byte> day) {
    DayCounter counter;
    EXPECT_TRUE(parse(day, counter).has_value());
    return size_books(counter.directory(), counter.stats(), SizingRules{});
}

} // namespace

TEST(SortedLevelsBetterThan, CountsTheLevelsAnInsertOrEraseShifts) {
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 10, .max_qty = Qty{100}};
    fast::SortedLevels levels{config};
    for (const std::int64_t p : {100, 102, 104}) {
        static_cast<void>(levels.get_or_create(Side::Buy, Price{p}));
        static_cast<void>(levels.get_or_create(Side::Sell, Price{p + 10}));
    }
    // Bids 100, 102, 104 (104 best). Asks 110, 112, 114 (110 best).
    EXPECT_EQ(levels.better_than(Side::Buy, Price{104}), 0U); // the best itself
    EXPECT_EQ(levels.better_than(Side::Buy, Price{105}), 0U); // would become the best
    EXPECT_EQ(levels.better_than(Side::Buy, Price{102}), 1U); // an existing level
    EXPECT_EQ(levels.better_than(Side::Buy, Price{101}), 2U); // a new one between two
    EXPECT_EQ(levels.better_than(Side::Buy, Price{1}), 3U);   // far below everything
    EXPECT_EQ(levels.better_than(Side::Sell, Price{110}), 0U);
    EXPECT_EQ(levels.better_than(Side::Sell, Price{113}), 2U); // asks: lower is better
    EXPECT_EQ(levels.better_than(Side::Sell, Price{999}), 3U);
}

TEST(FlowAnalysis, CountsEveryLevelChangeAndDistanceOfAHandMadeDay) {
    const std::vector<std::byte> day = flow_day();
    BookBuilder<FastBook> builder{configs_for(day), BuilderOptions{}};
    FlowAnalysis flow{builder};
    ASSERT_TRUE(parse(day, flow).has_value());
    const FlowStats &s = flow.stats();

    EXPECT_EQ(s.created[0], 3U); // refs 1, 2 and 6
    EXPECT_EQ(s.created[2], 2U); // ref 3 (shift 2) and ref 5 (shift 3): bucket 2-3
    EXPECT_EQ(s.created_shifted, 0U + 0 + 2 + 3 + 0);
    EXPECT_EQ(s.removed[0], 1U); // 100,100 when ref 2 is replaced
    EXPECT_EQ(s.removed[1], 1U); // 100,000 when ref 4 cancels all
    EXPECT_EQ(s.removed[2], 1U); // 99,000 when ref 3 is deleted
    EXPECT_EQ(s.removed_shifted, 2U + 1 + 0);

    const auto distance = [&](Distance d) { return s.distance[static_cast<std::size_t>(d)]; };
    EXPECT_EQ(distance(Distance::EmptySide), 1U);
    EXPECT_EQ(distance(Distance::ImprovesBest), 2U); // refs 2 and 6
    EXPECT_EQ(distance(Distance::AtBest), 0U);
    EXPECT_EQ(distance(Distance::Within10Cents), 1U);    // ref 4
    EXPECT_EQ(distance(Distance::Within1Dollar), 1U);    // ref 3
    EXPECT_EQ(distance(Distance::Within100Dollars), 1U); // ref 5, the stub quote

    // Every model saw the day's 10 lookups: 5 adds, a delete, an execute, a cancel, and a
    // replace's old and new price. Only the bid side was used, so one window was placed.
    for (const WindowModel &w : flow.windows()) {
        EXPECT_EQ(w.stats().lookups, 10U);
        EXPECT_EQ(w.stats().placements, 1U);
    }

    // The analysis only watches: the books are what a plain replay builds.
    BookBuilder<FastBook> plain{configs_for(day), BuilderOptions{}};
    ASSERT_TRUE(parse(day, plain).has_value());
    EXPECT_EQ(compare_books(builder.book(1), plain.book(1)), std::nullopt);
    EXPECT_EQ(builder.book(1).level_orders(Side::Buy, Price{100'200}), std::vector{OrderId{6}});
    EXPECT_EQ(builder.book(1).level_orders(Side::Buy, Price{1}), std::vector{OrderId{5}});
}

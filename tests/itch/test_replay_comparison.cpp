/*
 * test_replay_comparison.cpp: tests for the two-book ITCH replay comparison (ITCH design §8.2).
 *
 * What it does: compare_books() must find nothing between identical copies, and must catch a
 *   different queue order, different remaining shares and a different level total. Then the
 *   golden day (support/itch_golden_day.hpp) replayed into the reference book and each fast book
 *   must agree after every message, with every stock compared in full at each System Event.
 *   Last, a deliberately undersized book on one side must be caught at the exact message where
 *   the two copies first differ.
 *
 * Why it exists: A comparison that can't fail proves nothing. Step 5 runs this on the real day;
 *   these tests show what it catches, on days small enough to reason about.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "itch_golden_day.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/reference/reference_book.hpp"

using namespace nanobook;
using namespace nanobook::itch;
using namespace nanobook::itch::golden;

namespace {

constexpr BookConfig kSmall{
    .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 10, .max_qty = Qty{1000}};

FeedAdd buy(std::uint64_t id, std::int64_t qty, std::int64_t price) {
    return FeedAdd{.id = OrderId{id}, .side = Side::Buy, .price = Price{price}, .qty = Qty{qty}};
}

std::vector<BookConfig> golden_configs() {
    DayCounter counter;
    const std::vector<std::byte> day = golden_day();
    EXPECT_TRUE(parse(day, counter).has_value());
    return size_books(counter.directory(), counter.stats(), SizingRules{});
}

template <typename Fast> void golden_day_agrees() {
    const std::vector<BookConfig> configs = golden_configs();
    BookBuilder<ReferenceBook> reference{configs, BuilderOptions{.check = true}};
    BookBuilder<Fast> fast{configs, BuilderOptions{.check = true}};
    ReplayComparison comparison{reference, fast, 1}; // full check after every order message
    const std::vector<std::byte> day = golden_day();
    const auto parsed = parse(day, comparison);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, day.size());
    comparison.finish();

    EXPECT_EQ(comparison.mismatches(), 0U)
        << comparison.first_mismatch().value_or(ComparisonMismatch{}).what;
    EXPECT_EQ(comparison.quick_checks(), 16U); // the 17 order messages but the one for locate 9
    // 16 single-stock checks, plus 2 stocks at each of the 6 System Events and at finish().
    EXPECT_EQ(comparison.full_checks(), 16U + 2 * 7);
}

} // namespace

TEST(CompareBooks, IdenticalCopiesAgree) {
    ReferenceBook a{kSmall};
    FastBook b{kSmall};
    for (const FeedAdd &add : {buy(1, 10, 100), buy(2, 20, 100), buy(3, 5, 99)}) {
        ASSERT_EQ(a.apply(add), FeedResult::Ok);
        ASSERT_EQ(b.apply(add), FeedResult::Ok);
    }
    EXPECT_EQ(compare_books(a, b), std::nullopt);
}

TEST(CompareBooks, CatchesADifferentQueueOrder) {
    ReferenceBook a{kSmall};
    FastBook b{kSmall};
    ASSERT_EQ(a.apply(buy(1, 10, 100)), FeedResult::Ok);
    ASSERT_EQ(a.apply(buy(2, 10, 100)), FeedResult::Ok);
    ASSERT_EQ(b.apply(buy(2, 10, 100)), FeedResult::Ok); // same orders, other way round
    ASSERT_EQ(b.apply(buy(1, 10, 100)), FeedResult::Ok);
    EXPECT_EQ(compare_books(a, b), "queue order at Buy 100");
}

TEST(CompareBooks, CatchesDifferentRemainingShares) {
    ReferenceBook a{kSmall};
    FastBook b{kSmall};
    ASSERT_EQ(a.apply(buy(1, 10, 100)), FeedResult::Ok);
    ASSERT_EQ(a.apply(buy(2, 20, 99)), FeedResult::Ok);
    ASSERT_EQ(b.apply(buy(1, 15, 100)), FeedResult::Ok); // same total at 100 after the reduce...
    ASSERT_EQ(b.apply(buy(2, 20, 99)), FeedResult::Ok);
    ASSERT_EQ(b.apply(FeedReduce{.id = OrderId{1}, .qty = Qty{5}}), FeedResult::Ok);
    EXPECT_EQ(compare_books(a, b), std::nullopt); // ...so these two agree
    ASSERT_EQ(b.apply(FeedReduce{.id = OrderId{2}, .qty = Qty{1}}), FeedResult::Ok);
    EXPECT_EQ(compare_books(a, b), "Buy levels (prices or totals)"); // 99: 20 vs 19
}

TEST(ReplayComparison, GoldenDayAgreesOnFastBook) {
    golden_day_agrees<FastBook>();
}

TEST(ReplayComparison, GoldenDayAgreesOnTickFastBook) {
    golden_day_agrees<TickFastBook>();
}

TEST(ReplayComparison, CatchesTheFirstMessageWhereTheCopiesDiffer) {
    std::vector<BookConfig> small = golden_configs();
    small[1].max_orders = 1; // AAA's fast copy has room for one order only
    BookBuilder<ReferenceBook> reference{golden_configs(), BuilderOptions{}};
    BookBuilder<FastBook> fast{small, BuilderOptions{}};
    ReplayComparison comparison{reference, fast, 0}; // quick checks and S events only
    const std::vector<std::byte> day = golden_day();
    ASSERT_TRUE(parse(day, comparison).has_value());
    comparison.finish();

    ASSERT_GT(comparison.mismatches(), 0U);
    const ComparisonMismatch first = comparison.first_mismatch().value_or(ComparisonMismatch{});
    // Messages 1-7 are O, R, R, S, H, H, Q; 8 adds ref 1; 9 adds ref 2: BookFull in the copy.
    EXPECT_EQ(first.message, 9U);
    EXPECT_EQ(first.locate, 1U);
    EXPECT_EQ(first.timestamp, 9U);
    EXPECT_EQ(first.what, "quick check (order count, best prices or their quantities)");
}

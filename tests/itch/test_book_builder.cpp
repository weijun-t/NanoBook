/*
 * test_book_builder.cpp: the counting pass, the sizing rules, and the golden replay (phase 4).
 *
 * What it does: First the counting pass (include/nanobook/itch/day_stats.hpp) on a small day:
 *   peak resting orders, price range, largest order, ignored inputs, and a re-sent directory
 *   entry. Then the sizing rules (sizing.hpp): exact margins, the default book, a locate with no
 *   stats. Last the golden replay (ITCH design §8.3): a made-up day with two stocks and every kind
 *   of message, including deliberate errors and two crossed moments, counted, sized and replayed
 *   into every book type. The expected final books and every statistic are written out below.
 *
 * Why it exists: The full Nasdaq day is too big for CI and can't go in the repo. This day is
 *   small enough to work out by hand, so each expected number can be checked line by line, and
 *   CI runs it on every commit.
 *
 * How it works: The bytes come from the test encoder (support/itch_encoder.hpp); the golden day
 *   itself is in support/itch_golden_day.hpp, shared with the differential replay test. They go
 *   through the real parser, then the counter, the sizing and the BookBuilder, as nanobook_replay
 *   does.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "itch_encoder.hpp"
#include "itch_golden_day.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "../test_helpers.hpp"

using namespace nanobook;
using namespace nanobook::itch;
using namespace nanobook::itch::golden;

namespace {

std::vector<OrderId> ids(std::initializer_list<std::uint64_t> values) {
    std::vector<OrderId> out;
    for (const std::uint64_t v : values) {
        out.push_back(OrderId{v});
    }
    return out;
}

template <typename Handler> void parse_all(std::span<const std::byte> bytes, Handler &handler) {
    const auto parsed = parse(bytes, handler);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(*parsed, bytes.size());
}

template <typename Book> class GoldenReplay : public ::testing::Test {};

} // namespace

TYPED_TEST_SUITE(GoldenReplay, test::BookTypes, test::BookTypeNames);

// ---------------------------------------------------------------------------
// Counting (day_stats.hpp)
// ---------------------------------------------------------------------------

TEST(DayCounter, CountsPeakOrdersPricesAndSizes) {
    Writer w;
    w.stock_directory({.locate = 1, .timestamp = 1, .stock = kAaa});
    w.add_order(add(2, 1, 1, 'B', 100, 50'000));
    w.add_order(add(3, 1, 2, 'S', 300, 52'000));
    w.add_order(add(4, 1, 3, 'B', 10, 49'000)); // 3 resting: the peak
    w.delete_order({.locate = 1, .timestamp = 5, .order_ref = 1});
    w.add_order(add(6, 1, 4, 'B', 20, 51'000)); // 3 again, not more
    w.executed({.locate = 1,
                .timestamp = 7,
                .order_ref = 2,
                .executed_shares = 300,
                .match_number = 1}); // fully executed: 2 left
    w.replace({.locate = 1,
               .timestamp = 8,
               .original_order_ref = 3,
               .new_order_ref = 5,
               .shares = 10,
               .price = 53'000});                                   // still 2
    w.delete_order({.locate = 1, .timestamp = 9, .order_ref = 99}); // unknown: ignored
    w.add_order(add(10, 1, 4, 'B', 20, 51'000));                    // already live: ignored

    DayCounter counter;
    parse_all(w.bytes(), counter);
    EXPECT_EQ(counter.stats().at(kAaa),
              (StockStats{
                  .peak_orders = 3, .min_price = 49'000, .max_price = 53'000, .max_shares = 300}));
    EXPECT_EQ(counter.ignored(), 2U);
    ASSERT_EQ(counter.directory().size(), 2U);
    EXPECT_EQ(counter.directory()[1], kAaa);
}

TEST(DayCounter, StatsAreKeyedBySymbolNotLocate) {
    Writer w;
    w.stock_directory({.locate = 7, .timestamp = 1, .stock = kBbb});
    w.add_order(add(2, 7, 1, 'S', 5, 10'000));
    DayCounter counter;
    parse_all(w.bytes(), counter);
    const std::map<Symbol, StockStats> stats = counter.stats();
    ASSERT_EQ(stats.size(), 1U);
    EXPECT_EQ(stats.begin()->first, kBbb); // tomorrow BBB may have another locate
    EXPECT_EQ(stats.begin()->second.peak_orders, 1U);
}

// ---------------------------------------------------------------------------
// Sizing (sizing.hpp)
// ---------------------------------------------------------------------------

TEST(Sizing, MarginsAreExactWholePercentages) {
    const BookConfig c = size_book(
        StockStats{.peak_orders = 3, .min_price = 100'000, .max_price = 100'100, .max_shares = 1},
        SizingRules{});
    EXPECT_EQ(c.max_orders, 20U);              // ceil(3 * 125%) = 4, + 16
    EXPECT_EQ(c.min_price, Price{90'000});     // 100,000 - 10%
    EXPECT_EQ(c.max_price, Price{110'110});    // 100,100 + 10%, exactly (no floating point)
    EXPECT_EQ(c.max_qty, Qty{kMaxItchShares}); // whatever Nasdaq accepted
    EXPECT_TRUE(c.is_valid());
}

TEST(Sizing, ClampsToTheSpecsPriceRange) {
    const BookConfig c = size_book(
        StockStats{.peak_orders = 1, .min_price = 1, .max_price = 2'000'000'000, .max_shares = 1},
        SizingRules{});
    EXPECT_EQ(c.min_price, Price{1});
    EXPECT_EQ(c.max_price, Price{kMaxItchPrice});
    EXPECT_TRUE(c.is_valid());
}

TEST(Sizing, AStockWithoutStatsGetsTheDefaultBook) {
    const SizingRules rules;
    EXPECT_EQ(size_book(StockStats{}, rules).max_orders, rules.default_orders); // no orders
    const std::vector<Symbol> directory{Symbol{}, kAaa, kBbb};
    const std::map<Symbol, StockStats> stats{
        {kAaa, StockStats{.peak_orders = 10, .min_price = 5, .max_price = 5, .max_shares = 1}}};
    const std::vector<BookConfig> configs = size_books(directory, stats, rules);
    ASSERT_EQ(configs.size(), 3U);
    EXPECT_EQ(configs[0].max_orders, kUnusedBook.max_orders); // locate 0 is never a stock
    EXPECT_EQ(configs[0].max_price, kUnusedBook.max_price);
    EXPECT_EQ(configs[1].max_orders, 29U);                  // ceil(10 * 125%) = 13, + 16
    EXPECT_EQ(configs[2].max_orders, rules.default_orders); // BBB: no stats (a new listing)
    EXPECT_EQ(configs[2].max_price, Price{kMaxItchPrice});  // any price
    for (const BookConfig &c : configs) {
        EXPECT_TRUE(c.is_valid());
    }
}

// ---------------------------------------------------------------------------
// The golden replay (design §8.3), on every book type
// ---------------------------------------------------------------------------

TYPED_TEST(GoldenReplay, RebuildsTheDayExactly) {
    const std::vector<std::byte> day = golden_day();

    DayCounter counter;
    parse_all(day, counter);
    const std::vector<BookConfig> configs =
        size_books(counter.directory(), counter.stats(), SizingRules{});
    BookBuilder<TypeParam> builder{configs, BuilderOptions{.check = true, .structure_every = 1}};
    parse_all(day, builder);

    // The final books.
    const TypeParam &aaa = builder.book(1);
    EXPECT_EQ(aaa.level_orders(Side::Buy, Price{100'000}), ids({1}));
    EXPECT_EQ(aaa.level_orders(Side::Buy, Price{100'050}), ids({4}));
    EXPECT_EQ(aaa.leaves(OrderId{1}), Qty{80});
    EXPECT_EQ(aaa.leaves(OrderId{4}), Qty{45});
    EXPECT_EQ(aaa.best_bid(), Price{100'050});
    EXPECT_EQ(aaa.best_ask(), std::nullopt);
    EXPECT_EQ(aaa.order_count(), 2U);
    const TypeParam &bbb = builder.book(2);
    EXPECT_EQ(bbb.level_orders(Side::Sell, Price{200'000}), ids({5}));
    EXPECT_EQ(bbb.best_bid(), std::nullopt);
    EXPECT_EQ(bbb.order_count(), 1U);
    EXPECT_EQ(builder.name(1), kAaa);
    EXPECT_EQ(builder.find(kBbb), 2U);

    // Every message, by type: 6 S, 3 R, 3 H, 7 A, 1 F, 2 E, 1 C, 2 X, 3 D, 1 U, 1 P, 1 I.
    const ReplayStats &s = builder.stats();
    const std::map<char, std::uint64_t> expected_messages{{'S', 6}, {'R', 3}, {'H', 3}, {'A', 7},
                                                          {'F', 1}, {'E', 2}, {'C', 1}, {'X', 2},
                                                          {'D', 3}, {'U', 1}, {'P', 1}, {'I', 1}};
    for (int type = 0; type < 256; ++type) {
        const auto found = expected_messages.find(static_cast<char>(type));
        EXPECT_EQ(s.messages[static_cast<std::size_t>(type)],
                  found == expected_messages.end() ? 0U : found->second)
            << "type " << static_cast<char>(type);
    }

    // Every book result. The add for locate 9 never reached a book.
    EXPECT_EQ(s.result(OrderMessage::Add, FeedResult::Ok), 5U);
    EXPECT_EQ(s.result(OrderMessage::Add, FeedResult::DuplicateOrder), 1U);
    EXPECT_EQ(s.result(OrderMessage::AddMpid, FeedResult::Ok), 1U);
    EXPECT_EQ(s.result(OrderMessage::Executed, FeedResult::Ok), 1U);
    EXPECT_EQ(s.result(OrderMessage::Executed, FeedResult::UnknownOrder), 1U);
    EXPECT_EQ(s.result(OrderMessage::ExecutedWithPrice, FeedResult::Ok), 1U);
    EXPECT_EQ(s.result(OrderMessage::Cancel, FeedResult::Ok), 1U);
    EXPECT_EQ(s.result(OrderMessage::Cancel, FeedResult::ReduceTooLarge), 1U);
    EXPECT_EQ(s.result(OrderMessage::Delete, FeedResult::Ok), 3U);
    EXPECT_EQ(s.result(OrderMessage::Replace, FeedResult::Ok), 1U);
    std::uint64_t total = 0;
    for (const auto &row : s.results) {
        for (const std::uint64_t n : row) {
            total += n;
        }
    }
    EXPECT_EQ(total, 16U); // the 17 order messages, minus the one for locate 9
    EXPECT_EQ(s.unknown_locate, 1U);
    EXPECT_EQ(s.bad_input, 0U);

    // The crossed moments, with the stock's state and the market's at that time.
    EXPECT_EQ(s.crossed_starts, 2U);
    EXPECT_EQ(s.crossed_while_trading, 1U); // the first: BBB was trading (T); the second halted
    EXPECT_EQ(s.crossed_messages, 2U);
    EXPECT_EQ(
        s.first_crossed[0],
        (CrossedMoment{.timestamp = 17, .locate = 2, .trading_state = 'T', .system_event = 'Q'}));
    EXPECT_EQ(
        s.first_crossed[1],
        (CrossedMoment{.timestamp = 25, .locate = 2, .trading_state = 'H', .system_event = 'Q'}));
    EXPECT_EQ(s.structure_checks, 16U); // structure_every = 1: after every order message
    EXPECT_EQ(s.structure_failures, 0U);
}

TYPED_TEST(GoldenReplay, AMessageForAStockWithoutABookIsCountedNotApplied) {
    Writer w;
    w.stock_directory({.locate = 1, .timestamp = 1, .stock = kAaa});
    w.add_order(add(2, 3, 1, 'B', 1, 1)); // the builder only has locates 0 to 3...
    w.add_order(add(3, 2, 2, 'B', 1, 1)); // ...and no R named locate 2
    w.add_order(add(4, 0, 3, 'B', 1, 1)); // locate 0 is never a stock
    w.add_order(add(5, 1, 0, 'B', 1, 1)); // ref 0 is "no id" in NanoBook
    w.add_order(add(6, 1, 4, 'X', 1, 1)); // side must be B or S
    w.add_order(add(7, 1, 5, 'B', 1, 1)); // the one that's fine
    const std::vector<BookConfig> configs(4, BookConfig{.min_price = Price{1},
                                                        .max_price = Price{100},
                                                        .max_orders = 4,
                                                        .max_qty = Qty{100}});
    BookBuilder<TypeParam> builder{configs, BuilderOptions{}};
    parse_all(w.bytes(), builder);
    EXPECT_EQ(builder.stats().unknown_locate, 3U);
    EXPECT_EQ(builder.stats().bad_input, 2U);
    EXPECT_EQ(builder.stats().result(OrderMessage::Add, FeedResult::Ok), 1U);
    EXPECT_EQ(builder.book(1).order_count(), 1U);
    EXPECT_EQ(builder.book(2).order_count(), 0U);
}

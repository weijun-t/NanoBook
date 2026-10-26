/*
 * test_watchlist.cpp: tests for what the viewer shows (support/watchlist.hpp), without a terminal.
 *
 * What it does: The helpers first: prices and times formatted as the screen shows them, an empty
 *   side as "-", the spread in basis points, a crossed book spotted, ITCH's padding trimmed. The
 *   latency percentiles from bucket counts, against hand-worked ranks. Then a Watchlist on a
 *   table written by the test: nothing listed while the table says Starting; once Running, every
 *   named stock listed; rates appear after a 1 s window and are the differences over it; sorting
 *   by activity, symbol and spread; the filter (any case); pinned stocks first; a slot the
 *   writer is in the middle of keeps its last value; and a new run starts everything over.
 *
 * Why it exists: The numbers on screen should be checked by something other than looking at
 *   them. The drawing itself is tested separately (test_view_render.cpp, viewer builds only).
 *
 * How it works: The table is on the heap, written with l1_table.hpp's writer functions; the
 *   test passes its own "now" to refresh(), so a 1 s window takes no real time.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "l1_table.hpp" // support/
#include "watchlist.hpp"

using namespace nanobook::view;
using nanobook::itch::Symbol;

namespace {

Symbol sym(const char *text) {
    Symbol s{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    for (std::size_t i = 0; i < s.size() && text[i] != '\0'; ++i) {
        s[i] = text[i];
    }
    return s;
}

Quote quote(std::int64_t bid, std::int64_t ask, std::uint64_t updates) {
    return Quote{.sequence = updates,
                 .timestamp = 0,
                 .bid_price = bid,
                 .bid_qty = 100,
                 .ask_price = ask,
                 .ask_qty = 200,
                 .updates = updates};
}

std::vector<std::string> symbols(const std::vector<Row> &rows) {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const Row &r : rows) {
        out.emplace_back(trimmed(r.symbol));
    }
    return out;
}

// A table with three named stocks, Running.
std::unique_ptr<L1Table> three_stocks() {
    auto t = std::make_unique<L1Table>();
    t->header.run_id.store(1);
    t->symbols[1] = sym("AAPL");
    t->symbols[2] = sym("MSFT");
    t->symbols[3] = sym("ZVZZT");
    write(t->slots[1], quote(1'650'500, 1'650'600, 10)); // 1 cent wide on $165
    write(t->slots[2], quote(4'000'000, 4'000'500, 5));  // 5 cents on $400
    write(t->slots[3], quote(100'000, 0, 1));            // no asks
    start(*t, 0);
    return t;
}

} // namespace

TEST(WatchlistFormat, PricesTimesAndSpreads) {
    EXPECT_EQ(format_price(1'650'500), "165.0500");
    EXPECT_EQ(format_price(5), "0.0005");
    EXPECT_EQ(format_price(0), "-");
    EXPECT_EQ(format_time(34'264'123'456'789), "09:31:04.123");
    EXPECT_EQ(trimmed(sym("AAPL")), "AAPL");
    EXPECT_EQ(trimmed(Symbol{}), ""); // an unused locate: all '\0'

    // $0.01 over a $165.055 mid.
    EXPECT_NEAR(spread_bps(quote(1'650'500, 1'650'600, 1)).value_or(-1.0), 0.6058, 1e-4);
    EXPECT_FALSE(spread_bps(quote(1'650'500, 0, 1)).has_value());
    EXPECT_TRUE(crossed(quote(101, 100, 1)));
    EXPECT_TRUE(crossed(quote(100, 100, 1)));
    EXPECT_FALSE(crossed(quote(100, 101, 1)));
    EXPECT_FALSE(crossed(quote(100, 0, 1))); // one-sided is not crossed
}

TEST(WatchlistFormat, LatencyPercentilesFromBuckets) {
    std::vector<std::uint64_t> counts(kLatencyBuckets, 0);
    EXPECT_FALSE(summarize_buckets(counts, 1'000'000'000).has_value()); // no samples
    counts[100] = 98;                                                   // 98 samples of 100 ticks
    counts[500] = 1;                                                    // 1 of 500
    counts[1'000] = 1;                                                  // 1 of 1,000
    // 1 tick = 1 ns. Ranks: p50 -> 50 (bucket 100), p99 -> 99 (500), p99.9 -> ceil(99.9) = 100.
    EXPECT_EQ(summarize_buckets(counts, 1'000'000'000),
              (LatencyWindow{.count = 100, .p50_ns = 100, .p99_ns = 500, .p999_ns = 1'000}));
    // At 2 ticks per ns every value halves.
    EXPECT_EQ(summarize_buckets(counts, 2'000'000'000),
              (LatencyWindow{.count = 100, .p50_ns = 50, .p99_ns = 250, .p999_ns = 500}));
}

TEST(Watchlist, ListsNothingWhileStarting) {
    auto t = std::make_unique<L1Table>();
    t->header.run_id.store(1);
    t->symbols[1] = sym("AAPL");
    Watchlist w;
    w.refresh(*t, 0.0);
    EXPECT_EQ(w.health().state, RunState::Starting);
    EXPECT_TRUE(w.rows("", SortBy::Symbol).empty());
}

TEST(Watchlist, ListsEveryNamedStockWithItsQuote) {
    auto t = three_stocks();
    Watchlist w;
    w.refresh(*t, 0.0);
    const auto rows = w.rows("", SortBy::Symbol);
    EXPECT_EQ(symbols(rows), (std::vector<std::string>{"AAPL", "MSFT", "ZVZZT"}));
    EXPECT_EQ(rows[0].quote, quote(1'650'500, 1'650'600, 10));
    EXPECT_EQ(rows[0].locate, 1U);
    EXPECT_EQ(w.health().state, RunState::Running);
}

TEST(Watchlist, RatesAreTheDifferencesOverAOneSecondWindow) {
    auto t = three_stocks();
    t->header.updates.store(16);
    t->header.itch_time.store(1'000'000'000);
    Watchlist w;
    w.refresh(*t, 10.0);                                 // opens the window
    write(t->slots[1], quote(1'650'500, 1'650'600, 40)); // +30
    write(t->slots[2], quote(4'000'000, 4'000'500, 15)); // +10
    t->header.updates.store(56);                         // +40
    t->header.itch_time.store(61'000'000'000);           // +60 ITCH seconds
    w.refresh(*t, 10.5);                                 // half a second: too soon, no rates yet
    EXPECT_EQ(w.health().updates_per_second, 0.0);
    w.refresh(*t, 12.0);                                   // 2 s after the window opened
    EXPECT_DOUBLE_EQ(w.health().updates_per_second, 20.0); // 40 / 2
    EXPECT_DOUBLE_EQ(w.health().speed, 30.0);              // 60 / 2
    EXPECT_TRUE(w.health().alive);
    const auto rows = w.rows("", SortBy::Activity);
    EXPECT_EQ(symbols(rows), (std::vector<std::string>{"AAPL", "MSFT", "ZVZZT"}));
    EXPECT_DOUBLE_EQ(rows[0].updates_per_second, 15.0);
    EXPECT_DOUBLE_EQ(rows[1].updates_per_second, 5.0);
    w.refresh(*t, 13.5); // a quiet window: rates back to 0
    EXPECT_FALSE(w.health().alive);
    EXPECT_EQ(w.rows("", SortBy::Activity)[0].updates_per_second, 0.0);
}

TEST(Watchlist, LatencyIsTheLastWindowsOnly) {
    auto t = three_stocks();
    t->header.ticks_per_second.store(1'000'000'000); // 1 tick = 1 ns
    for (int i = 0; i < 10; ++i) {
        record_latency(*t, 900); // before the window: must not count
    }
    Watchlist w;
    w.refresh(*t, 0.0);
    for (int i = 0; i < 4; ++i) {
        record_latency(*t, 300);
    }
    w.refresh(*t, 1.0);
    EXPECT_EQ(w.health().latency,
              (LatencyWindow{.count = 4, .p50_ns = 300, .p99_ns = 300, .p999_ns = 300}));
    EXPECT_TRUE(w.health().paced);
}

TEST(Watchlist, SortsByActivitySymbolAndSpread) {
    auto t = three_stocks();
    Watchlist w;
    w.refresh(*t, 0.0);
    // Activity with no window yet: by updates so far (10, 5, 1).
    EXPECT_EQ(symbols(w.rows("", SortBy::Activity)),
              (std::vector<std::string>{"AAPL", "MSFT", "ZVZZT"}));
    // Spread: AAPL 0.61 bps, MSFT 1.25 bps, ZVZZT one-sided (last).
    write(t->slots[1], quote(1'650'000, 1'660'000, 11)); // now $1 wide: 60 bps
    w.refresh(*t, 0.1);
    EXPECT_EQ(symbols(w.rows("", SortBy::Spread)),
              (std::vector<std::string>{"MSFT", "AAPL", "ZVZZT"}));
}

TEST(Watchlist, FiltersByPrefixInAnyCaseAndPinsFirst) {
    auto t = three_stocks();
    Watchlist w;
    w.refresh(*t, 0.0);
    EXPECT_EQ(symbols(w.rows("ms", SortBy::Symbol)), (std::vector<std::string>{"MSFT"}));
    EXPECT_TRUE(w.rows("Q", SortBy::Symbol).empty());
    w.toggle_pin(3);
    EXPECT_EQ(symbols(w.rows("", SortBy::Symbol)),
              (std::vector<std::string>{"ZVZZT", "AAPL", "MSFT"}));
    EXPECT_TRUE(w.rows("", SortBy::Symbol)[0].pinned);
    w.toggle_pin(3);
    EXPECT_EQ(symbols(w.rows("", SortBy::Symbol))[0], "AAPL");
}

TEST(Watchlist, ASlotBeingWrittenKeepsItsLastValue) {
    auto t = three_stocks();
    Watchlist w;
    w.refresh(*t, 0.0);
    t->slots[1].seq.store(t->slots[1].seq.load() + 1); // odd: the writer in the middle
    t->slots[1].bid_price.store(1);                    // half-written garbage
    w.refresh(*t, 0.1);
    EXPECT_EQ(w.rows("AAPL", SortBy::Symbol)[0].quote.bid_price, 1'650'500);
}

TEST(Watchlist, ANewRunStartsEverythingOver) {
    auto t = three_stocks();
    Watchlist w;
    w.refresh(*t, 0.0);
    w.toggle_pin(1);                         // AAPL, at locate 1
    auto next = std::make_unique<L1Table>(); // another run: another stock at locate 1
    next->header.run_id.store(2);
    next->symbols[1] = sym("SPY");
    start(*next, 0);
    w.refresh(*next, 1.0);
    const auto rows = w.rows("", SortBy::Symbol);
    EXPECT_EQ(symbols(rows), (std::vector<std::string>{"SPY"}));
    EXPECT_FALSE(rows[0].pinned); // the pin was AAPL's, not SPY's
    EXPECT_EQ(rows[0].quote, Quote{});
}

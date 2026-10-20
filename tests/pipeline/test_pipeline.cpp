/*
 * test_pipeline.cpp: tests for the records and the three-thread pipeline (pipeline design §7).
 *
 * What it does: First the records: every message kind encoded into a FeedRecord and decoded back
 *   gives the book builder exactly the fields it reads. Then the pipeline against the same work on
 *   one thread (run_sequential): on the golden day (hand-checked, phase 4) and on a busy day with
 *   2-record rings (constantly full and empty, so the threads hand over on almost every record),
 *   the stream of TopOfBook updates must be identical, update for update, and the final books
 *   identical to a plain phase 4 replay. Last, a broken message mid-stream: every thread must
 *   still stop, and the pipeline must say the input was broken.
 *
 * Why it exists: Threads must change nothing but speed. A lost, repeated or reordered record, or
 *   a field dropped on the way through a ring, would quietly change the books. These tests run
 *   under ThreadSanitizer in CI, which also checks every hand-over between the threads.
 *
 * How it works: The sink collects the updates in a vector (reserved first). The final books are
 *   compared stock by stock with phase 4's compare_books(). Under TSan the busy day is smaller.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "itch_golden_day.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/pipeline/records.hpp"
#include "nanobook/pipeline/stages.hpp"

using namespace nanobook;
using namespace nanobook::itch;
using namespace nanobook::pipeline;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

using Builder = BookBuilder<FastBook>;

std::vector<BookConfig> configs_for(std::span<const std::byte> day) {
    DayCounter counter;
    EXPECT_TRUE(parse(day, counter).has_value());
    return size_books(counter.directory(), counter.stats(), SizingRules{});
}

// A ReadFeed for run_pipeline/run_sequential: one parse() of the whole buffer.
auto whole(std::span<const std::byte> day) {
    return [day](auto &handler) {
        const auto parsed = parse(day, handler);
        return parsed.has_value() && *parsed == day.size();
    };
}

struct Outcome {
    PipelineResult result;
    std::vector<TopOfBook> updates;
};

Outcome sequential(std::span<const std::byte> day, Builder &builder) {
    Outcome run;
    run.updates.reserve(day.size() / 19);
    auto sink = [&run](const TopOfBook &t) { run.updates.push_back(t); };
    run.result = run_sequential(whole(day), builder, sink);
    return run;
}

template <std::size_t Capacity = kRingCapacity>
Outcome threaded(std::span<const std::byte> day, Builder &builder) {
    Outcome run;
    run.updates.reserve(day.size() / 19); // at most one update per message (19+2 bytes or more)
    auto sink = [&run](const TopOfBook &t) { run.updates.push_back(t); };
    run.result = run_pipeline<Capacity>(whole(day), builder, sink, PipelineOptions{});
    return run;
}

// The pipeline's books against a plain phase 4 replay of the same day, stock by stock.
void expect_same_books(std::span<const std::byte> day, const Builder &piped) {
    Builder plain{configs_for(day), BuilderOptions{}};
    ASSERT_TRUE(parse(day, plain).has_value());
    EXPECT_EQ(piped.stats().results, plain.stats().results);
    ASSERT_EQ(piped.locates(), plain.locates());
    for (std::size_t i = 1; i < plain.locates(); ++i) {
        const auto locate = static_cast<std::uint16_t>(i);
        EXPECT_EQ(compare_books(piped.book(locate), plain.book(locate)), std::nullopt) << locate;
    }
}

// Captures what decode() hands the builder.
struct Capture {
    AddOrder add{};
    OrderExecuted executed{};
    OrderExecutedWithPrice executed_with_price{};
    OrderCancel cancel{};
    OrderDelete del{};
    OrderReplace replace{};
    StockDirectory directory{};
    StockTradingAction action{};
    SystemEvent event{};
    void on(const AddOrder &m) {
        add = m;
    }
    void on(const OrderExecuted &m) {
        executed = m;
    }
    void on(const OrderExecutedWithPrice &m) {
        executed_with_price = m;
    }
    void on(const OrderCancel &m) {
        cancel = m;
    }
    void on(const OrderDelete &m) {
        del = m;
    }
    void on(const OrderReplace &m) {
        replace = m;
    }
    void on(const StockDirectory &m) {
        directory = m;
    }
    void on(const StockTradingAction &m) {
        action = m;
    }
    void on(const SystemEvent &m) {
        event = m;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The records
// ---------------------------------------------------------------------------

TEST(FeedRecords, CarryEveryFieldTheBookBuilderReads) {
    Capture c;
    AddOrder f = golden::add(5, 14, 77, 'S', 300, 1'650'500);
    f.attribution = golden::kNsdq;
    decode(encode(f), c);
    EXPECT_EQ(c.add, f); // locate, time, ref, side, shares, price, attribution (stock: unused)
    EXPECT_EQ(encode(f).kind, FeedKind::AddMpid);
    EXPECT_EQ(encode(golden::add(5, 14, 78, 'B', 1, 1)).kind, FeedKind::Add);

    const OrderReplace u{.locate = 14,
                         .timestamp = 6,
                         .original_order_ref = 77,
                         .new_order_ref = 0xFFFF'FFFF'FFFF,
                         .shares = 4'000'000'000,
                         .price = 2'000'000'000};
    decode(encode(u), c);
    EXPECT_EQ(c.replace, u);

    const OrderCancel x{.locate = 14, .timestamp = 7, .order_ref = 77, .cancelled_shares = 5};
    decode(encode(x), c);
    EXPECT_EQ(c.cancel, x);
    const OrderDelete d{.locate = 14, .timestamp = 8, .order_ref = 78};
    decode(encode(d), c);
    EXPECT_EQ(c.del, d);

    // Executions: match number and price aren't read by the builder, so they don't travel.
    decode(encode(OrderExecuted{.locate = 14,
                                .timestamp = 9,
                                .order_ref = 77,
                                .executed_shares = 30,
                                .match_number = 123}),
           c);
    EXPECT_EQ(c.executed.order_ref, 77U);
    EXPECT_EQ(c.executed.executed_shares, 30U);
    decode(encode(OrderExecutedWithPrice{.locate = 14,
                                         .timestamp = 10,
                                         .order_ref = 77,
                                         .executed_shares = 20,
                                         .match_number = 1,
                                         .printable = 'Y',
                                         .execution_price = 5}),
           c);
    EXPECT_EQ(c.executed_with_price.executed_shares, 20U);

    decode(encode(StockDirectory{.locate = 14, .timestamp = 1, .stock = golden::kAaa}), c);
    EXPECT_EQ(c.directory.stock, golden::kAaa); // 8 characters in the `ref` field
    decode(encode(StockTradingAction{
               .locate = 14, .timestamp = 2, .stock = golden::kAaa, .trading_state = 'H'}),
           c);
    EXPECT_EQ(c.action.trading_state, 'H');
    decode(encode(SystemEvent{.locate = 0, .timestamp = 3, .event_code = 'Q'}), c);
    EXPECT_EQ(c.event.event_code, 'Q');
}

// ---------------------------------------------------------------------------
// Three threads give exactly what one thread gives
// ---------------------------------------------------------------------------

TEST(Pipeline, MatchesOneThreadOnTheGoldenDay) {
    const std::vector<std::byte> day = golden::golden_day();
    Builder one{configs_for(day), BuilderOptions{}};
    const Outcome expected = sequential(day, one);
    Builder three{configs_for(day), BuilderOptions{}};
    const Outcome got = threaded(day, three);

    EXPECT_TRUE(got.result.parsed);
    EXPECT_EQ(got.updates, expected.updates); // every update, in order, every field
    EXPECT_EQ(got.result.records, 29U);       // 17 order messages, 6 S, 3 R, 3 H
    EXPECT_EQ(got.result.skipped, 2U);        // P and I
    EXPECT_EQ(got.result.updates, 16U);       // order messages for a stock with a book
    EXPECT_EQ(got.result.updates, expected.updates.size());
    expect_same_books(day, three);
    // The last update: BBB after ref 8's delete, bid side empty, ask 10 @ $20.00.
    ASSERT_FALSE(got.updates.empty());
    EXPECT_EQ(got.updates.back(), (TopOfBook{.sequence = 16,
                                             .timestamp = 26,
                                             .bid_price = 0,
                                             .bid_qty = 0,
                                             .ask_price = 200'000,
                                             .ask_qty = 10,
                                             .sent = 0,
                                             .locate = 2,
                                             .end = false}));
}

TEST(Pipeline, MatchesOneThreadWithTwoRecordRings) {
    // 2-record rings: full after two pushes, empty after two pops, so the three threads wait on
    // each other on almost every record.
    const std::vector<std::byte> day = golden::busy_day(kThreadSanitizer ? 5'000 : 50'000);
    Builder one{configs_for(day), BuilderOptions{}};
    const Outcome expected = sequential(day, one);
    Builder three{configs_for(day), BuilderOptions{}};
    const Outcome got = threaded<2>(day, three);

    EXPECT_TRUE(got.result.parsed);
    EXPECT_EQ(got.updates.size(), expected.updates.size());
    EXPECT_TRUE(got.updates == expected.updates); // not EXPECT_EQ: don't print 100,000 updates
    EXPECT_GT(got.result.feed_waits + got.result.engine_waits, 0U); // the rings really filled
    expect_same_books(day, three);
}

TEST(Pipeline, ABrokenMessageStillStopsEveryThread) {
    std::vector<std::byte> day = golden::golden_day();
    Writer broken;
    broken.malformed(19, 'Z');
    // Put the broken message after the first 10 messages: 7 before the first add, then 3 adds.
    std::vector<std::byte> head;
    std::size_t pos = 0;
    for (int i = 0; i < 10; ++i) {
        pos += 2 + std::to_integer<std::size_t>(day[pos + 1]); // the length's low byte (< 256)
    }
    head.assign(day.begin(), day.begin() + static_cast<std::ptrdiff_t>(pos));
    head.insert(head.end(), broken.bytes().begin(), broken.bytes().end());
    head.insert(head.end(), day.begin() + static_cast<std::ptrdiff_t>(pos), day.end());

    Builder three{configs_for(day), BuilderOptions{}};
    const Outcome got = threaded(head, three); // returning at all means every thread stopped
    EXPECT_FALSE(got.result.parsed);
    EXPECT_EQ(got.result.records, 10U); // the messages before the broken one
    EXPECT_EQ(got.updates.size(), 3U);  // the three adds
}

/*
 * test_alloc.cpp: zero-allocation tests (README rule 1).
 *
 * What it does: First checks the allocation counter on code whose allocations are known (one new
 *   counts 1, none counts 0). Then points it at engine code: the EventBuffer, the fast book's
 *   parts, and the whole fast book (100,000 random operations after a warm-up) must never allocate
 *   after construction, and the reference book, which may allocate, must be seen allocating. The
 *   fast books must not allocate while applying 100,000 random feed inputs either, nor while
 *   replaying a made-up ITCH day through the parser and the book builder (phase 4), nor while
 *   records flow through the three-thread pipeline (phase 5).
 *   Finally the latency harness's timing loop and recorder must not allocate either, and no
 *   scenario may allocate while it times a fast book (phase 3).
 *
 * Why it exists: Phase 2's exit test is "the fast book allocates exactly 0 times while trading".
 *   Testing the counter in both directions means it can't be wrong either way unnoticed.
 *
 * Trade-offs: It is its own program, built only without sanitizers, because it replaces the global
 *   operator new for the whole program. The compiler may delete an allocation whose memory is
 *   never used, so the tests mark memory as used with keep().
 */

#include <gtest/gtest.h>
#include "alloc_counter.hpp"
#include "binary_log.hpp"
#include "differential_fuzz.hpp"
#include "feed_fuzz.hpp"
#include "itch_golden_day.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/pipeline/stages.hpp"
#include "latency_recorder.hpp"
#include "latency_scenarios.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/fast/id_table.hpp"
#include "nanobook/fast/intrusive_queue.hpp"
#include "nanobook/fast/order_pool.hpp"
#include "test_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

using namespace nanobook;
using namespace nanobook::test;
using support::allocations_during;

namespace {

// C++ lets the compiler delete a new/delete pair whose result is never used, and Clang does at -O3.
// That would make "one new counts 1" see 0. This empty assembly statement claims to read `p`, so
// the compiler has to keep the allocation. It generates no instructions.
void keep(const void *p) {
    asm volatile("" : : "r"(p) : "memory");
}

} // namespace

// ---------------------------------------------------------------------------
// The tool itself
// ---------------------------------------------------------------------------

TEST(AllocCounter, CountsOneForOneOperatorNew) {
    const std::uint64_t n = allocations_during([] {
        void *p = ::operator new(64);
        keep(p);
        ::operator delete(p);
    });
    EXPECT_EQ(n, 1U);
}

TEST(AllocCounter, CountsArrayNew) {
    const std::uint64_t n = allocations_during([] {
        auto *p = new int[16];
        keep(p);
        delete[] p;
    });
    EXPECT_EQ(n, 1U);
}

TEST(AllocCounter, CountsEveryAllocation) {
    const std::uint64_t n = allocations_during([] {
        for (int i = 0; i < 3; ++i) {
            auto p = std::make_unique<std::array<int, 8>>();
            keep(p.get());
        }
    });
    EXPECT_EQ(n, 3U);
}

TEST(AllocCounter, CountsVectorGrowth) {
    const std::uint64_t n = allocations_during([] {
        std::vector<int> v;
        v.reserve(4); // one allocation
        // Without this, Clang -O3 sees the first buffer is never used and skips allocating it
        // (legal C++), so it would count 1, not 2.
        keep(v.data());
        v.reserve(64); // grows: a second one
        keep(v.data());
    });
    EXPECT_EQ(n, 2U);
}

TEST(AllocCounter, CountsZeroWhenNothingIsAllocated) {
    const std::uint64_t n = allocations_during([] {
        std::array<std::int64_t, 64> a{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<std::int64_t>(i * i);
        }
        keep(a.data());
    });
    EXPECT_EQ(n, 0U);
}

// ---------------------------------------------------------------------------
// Engine code
// ---------------------------------------------------------------------------

// The EventBuffer promise (design §12): all memory up front, nothing while it's used.
TEST(ZeroAlloc, EventBufferNeverAllocatesAfterConstruction) {
    EventBuffer out{1000};
    const std::uint64_t n = allocations_during([&] {
        for (int round = 0; round < 10; ++round) {
            out.clear();
            for (std::uint64_t i = 1; i <= 1000; ++i) {
                out.push(Event::make_new(OrderId{i}));
            }
        }
        keep(&out);
    });
    EXPECT_EQ(n, 0U);
}

// The fast book's building blocks (fast-book design §3, §4.2): all memory in the pool's
// constructor, then nothing, however many orders come and go.
TEST(ZeroAlloc, OrderPoolAndQueuesNeverAllocateAfterConstruction) {
    fast::OrderPool pool{1000};
    std::array<fast::IntrusiveQueue, 4> levels{};
    const std::uint64_t n = allocations_during([&] {
        for (int round = 0; round < 50; ++round) {
            for (std::uint32_t i = 0; i < 1000; ++i) { // fill the pool, spread over 4 queues
                const std::uint32_t slot = pool.acquire();
                levels[i % levels.size()].push_back(pool, slot);
            }
            for (fast::IntrusiveQueue &q : levels) { // empty every queue from the front
                while (!q.empty()) {
                    const std::uint32_t slot = q.front();
                    q.unlink(pool, slot);
                    pool.release(slot);
                }
            }
        }
        keep(&pool);
    });
    EXPECT_EQ(n, 0U);
    EXPECT_EQ(pool.used(), 0U);
}

TEST(ZeroAlloc, IdTableNeverAllocatesAfterConstruction) {
    fast::IdTable table{1000};
    std::vector<OrderId> ids(1000); // what the pool would know: the id in each slot
    const auto id_of = [&ids](std::uint32_t slot) { return ids[slot]; };
    const std::uint64_t n = allocations_during([&] {
        std::uint64_t next = 1;
        for (int round = 0; round < 50; ++round) {
            const std::uint64_t first = next;
            for (std::uint32_t i = 0; i < 1000; ++i) {
                ids[i] = OrderId{next};
                table.insert(OrderId{next++}, i);
            }
            for (std::uint64_t id = first; id < next; ++id) {
                table.erase(OrderId{id}, id_of);
            }
        }
        keep(&table);
    });
    EXPECT_EQ(n, 0U);
    EXPECT_EQ(table.size(), 0U);
}

// Phase 2's exit test (README rule 1, fast-book design §8.4): the WHOLE fast book allocates
// nothing while trading. Build it (allocations allowed: start-up), warm it up with a random
// stream, then 100,000 more random orders, cancels and modifies must make exactly 0 allocations.
template <typename Book> std::uint64_t allocations_while_trading(std::uint32_t max_orders) {
    fuzz::Generator gen{2024, max_orders};
    Book book{gen.config()};
    EventBuffer out{gen.config().max_events_per_input()};
    const auto trade = [&](int count) {
        for (int i = 0; i < count; ++i) {
            fuzz::apply(book, gen.next(book), out);
            gen.observe(out.view());
        }
    };
    trade(20'000); // warm-up
    const std::uint64_t n = allocations_during([&] { trade(100'000); });
    EXPECT_TRUE(book.invariants_hold());
    return n;
}

TEST(ZeroAlloc, FastBookNeverAllocatesWhileTrading) {
    EXPECT_EQ(allocations_while_trading<FastBook>(2000), 0U);
    EXPECT_EQ(allocations_while_trading<FastBook>(16), 0U); // a tiny, often full book
}

TEST(ZeroAlloc, TickFastBookNeverAllocatesWhileTrading) {
    EXPECT_EQ(allocations_while_trading<TickFastBook>(2000), 0U);
}

TEST(ZeroAlloc, WindowFastBooksNeverAllocateWhileTrading) {
    EXPECT_EQ(allocations_while_trading<WindowFastBook>(2000), 0U);
    EXPECT_EQ(allocations_while_trading<Window8FastBook>(2000), 0U); // slides all the time
}

// Phase 4: copying a book from a feed (add, reduce, remove, replace) must not allocate either. The
// inputs are made first; only applying them is counted. max_orders 64 with 128 ids keeps the book
// often full, so BookFull and every other failure path run too.
template <typename Book> std::uint64_t allocations_while_applying_the_feed() {
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 64, .max_qty = Qty{100}};
    Book book{config};
    const std::vector<support::FeedInput> inputs = support::random_feed({.count = 100'000});
    std::uint64_t ok = 0;
    const std::uint64_t n = allocations_during([&] {
        for (const support::FeedInput &input : inputs) {
            ok += support::apply(book, input) == FeedResult::Ok ? 1U : 0U;
        }
    });
    EXPECT_GT(ok, 10'000U); // most of the work really happened
    EXPECT_TRUE(book.structure_holds());
    return n;
}

TEST(ZeroAlloc, FastBooksNeverAllocateWhileApplyingTheFeed) {
    EXPECT_EQ(allocations_while_applying_the_feed<FastBook>(), 0U);
    EXPECT_EQ(allocations_while_applying_the_feed<TickFastBook>(), 0U);
    EXPECT_EQ(allocations_while_applying_the_feed<WindowFastBook>(), 0U);
    EXPECT_EQ(allocations_while_applying_the_feed<Window8FastBook>(), 0U);
}

// The whole replay path (parser -> book builder -> books, ITCH design §6) on a made-up day of
// 3 stocks: counting and sizing first, then only the replay is counted, with checks on.
template <typename Book> std::uint64_t allocations_while_replaying_itch() {
    const std::vector<std::byte> day = itch::golden::busy_day(20'000);
    itch::DayCounter counter;
    EXPECT_TRUE(itch::parse(day, counter).has_value());
    const std::vector<BookConfig> configs =
        itch::size_books(counter.directory(), counter.stats(), itch::SizingRules{});
    itch::BookBuilder<Book> builder{configs,
                                    itch::BuilderOptions{.check = true, .structure_every = 0}};

    const std::uint64_t n =
        allocations_during([&] { EXPECT_TRUE(itch::parse(day, builder).has_value()); });
    EXPECT_EQ(builder.stats().result(itch::OrderMessage::Add, FeedResult::Ok), 20'000U);
    EXPECT_EQ(builder.stats().result(itch::OrderMessage::Replace, FeedResult::Ok), 5'000U);
    return n;
}

TEST(ZeroAlloc, ItchReplayNeverAllocates) {
    EXPECT_EQ(allocations_while_replaying_itch<FastBook>(), 0U);
    EXPECT_EQ(allocations_while_replaying_itch<TickFastBook>(), 0U);
    EXPECT_EQ(allocations_while_replaying_itch<WindowFastBook>(), 0U);
    EXPECT_EQ(allocations_while_replaying_itch<Window8FastBook>(), 0U);
}

// Phase 5: the publisher's binary log. Its buffer is made in the constructor; appending (and the
// write() system calls when the buffer fills) must not allocate.
TEST(ZeroAlloc, BinaryLogNeverAllocatesWhileAppending) {
    pipeline::BinaryLog log{"/dev/null", true};
    const std::uint64_t n = allocations_during([&] {
        for (std::uint64_t i = 1; i <= 100'000; ++i) { // 5.6 MB: about five buffer flushes
            log.append(pipeline::TopOfBook{.sequence = i, .bid_price = 1, .ask_price = 2});
        }
    });
    log.close();
    EXPECT_TRUE(log.ok());
    EXPECT_EQ(log.records(), 100'000U);
    EXPECT_EQ(n, 0U);
}

// Phase 5: the three-thread pipeline. Rings, books and threads are made before records flow;
// run_pipeline counts allocations from the moment all three threads are ready to the moment
// all three have finished, across every thread.
TEST(ZeroAlloc, PipelineNeverAllocatesWhileRecordsFlow) {
    const std::vector<std::byte> day = itch::golden::busy_day(20'000);
    itch::DayCounter counter;
    ASSERT_TRUE(itch::parse(day, counter).has_value());
    itch::BookBuilder<FastBook> builder{
        itch::size_books(counter.directory(), counter.stats(), itch::SizingRules{}),
        itch::BuilderOptions{}};
    std::uint64_t updates = 0;
    std::uint64_t checksum = 0; // the sink only adds numbers: it can't allocate
    auto sink = [&](const pipeline::TopOfBook &t) {
        ++updates;
        checksum += static_cast<std::uint64_t>(t.bid_price + t.ask_price);
    };
    const pipeline::PipelineResult result = pipeline::run_pipeline(
        [&day](auto &handler) { return itch::parse(day, handler).has_value(); }, builder, sink,
        pipeline::PipelineOptions{.allocations = &support::allocations});
    EXPECT_TRUE(result.parsed);
    EXPECT_EQ(result.updates, updates);
    EXPECT_GT(updates, 20'000U);
    EXPECT_NE(checksum, 0U);
    EXPECT_EQ(result.allocations, 0U);
}

// The reference book is allowed to allocate (it uses std::map and std::list). This proves the tool
// sees allocations made deep inside engine code. The fast book's version of this test (phase 2)
// expects exactly 0.
TEST(ZeroAlloc, ReferenceBookAllocatesWhenAnOrderRests) {
    constexpr BookConfig config{Price{1}, Price{1000}, 100, Qty{1'000'000}};
    ReferenceBook book{config};
    EventBuffer out{config.max_events_per_input()};

    const std::uint64_t n = allocations_during([&] { book.handle(buy(1, 1, 10, 10), out); });
    EXPECT_GT(n, 0U); // a new price level and a new order: 3 tree/list nodes today
}

// ---------------------------------------------------------------------------
// The latency harness (phase 3): its timing loop must obey rule 1 too
// ---------------------------------------------------------------------------

TEST(ZeroAlloc, LatencyRecorderNeverAllocatesWhileRecording) {
    constexpr std::size_t kOps = 100'000;
    latency::Recorder recorder{kOps}; // allocates here, before timing
    std::uint64_t sum = 0;
    const std::uint64_t n = allocations_during([&] {
        latency::time_each(
            recorder, kOps, [&](std::size_t i) { sum += i; },
            [](std::size_t) { return latency::Tag::Other; });
    });
    EXPECT_EQ(n, 0U);
    EXPECT_EQ(recorder.size() + recorder.dropped(), kOps);
    keep(&sum);
}

TEST(ZeroAlloc, LatencyScenariosNeverAllocateWhileTimingAFastBook) {
    constexpr std::size_t kOps = 2'000;
    const latency::Options options{.ops = kOps, .allocations = &support::allocations};
    for (const latency::Scenario s : latency::kAllScenarios) {
        SCOPED_TRACE(latency::scenario_name(s));
        latency::Recorder recorder{kOps};
        EXPECT_EQ(latency::run_scenario<FastBook>(s, recorder, options).timed_allocations, 0U);
        recorder.clear();
        EXPECT_EQ(latency::run_scenario<TickFastBook>(s, recorder, options).timed_allocations, 0U);
        latency::Options throughput = options;
        throughput.throughput = true;
        EXPECT_EQ(latency::run_scenario<FastBook>(s, recorder, throughput).timed_allocations, 0U);
    }
}

TEST(ZeroAlloc, LatencyScenariosSeeTheReferenceBookAllocate) {
    // Proves the count reaches the timed loop: the reference book allocates for every rest.
    constexpr std::size_t kOps = 2'000;
    latency::Recorder recorder{kOps};
    const latency::Options options{.ops = kOps, .allocations = &support::allocations};
    EXPECT_GE(latency::run_scenario<ReferenceBook>(latency::Scenario::Add, recorder, options)
                  .timed_allocations,
              kOps);
}

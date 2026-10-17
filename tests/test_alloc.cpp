/*
 * test_alloc.cpp: zero-allocation tests (README rule 1).
 *
 * What it does: First checks the allocation counter on code whose allocations are known (one new
 *   counts 1, none counts 0). Then points it at engine code: the EventBuffer, the fast book's
 *   parts, and the whole fast book (100,000 random operations after a warm-up) must never allocate
 *   after construction, and the reference book, which may allocate, must be seen allocating.
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
#include "differential_fuzz.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/fast/id_table.hpp"
#include "nanobook/fast/intrusive_queue.hpp"
#include "nanobook/fast/order_pool.hpp"
#include "test_helpers.hpp"

#include <array>
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
    const std::uint64_t n = allocations_during([&] {
        std::uint64_t next = 1;
        for (int round = 0; round < 50; ++round) {
            const std::uint64_t first = next;
            for (std::uint32_t i = 0; i < 1000; ++i) {
                table.insert(OrderId{next++}, i);
            }
            for (std::uint64_t id = first; id < next; ++id) {
                table.erase(OrderId{id});
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

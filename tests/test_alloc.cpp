#include <gtest/gtest.h>
#include "alloc_counter.hpp"
#include "test_helpers.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

// Zero-allocation tests (README rule 1, fast-book design §8.4). This is its own program, built
// only without sanitizers, because it replaces the global operator new to count calls.
//
// First the tool is checked on code whose allocations are known. Then it is pointed at engine code.

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

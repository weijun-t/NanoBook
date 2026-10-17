/*
 * test_order_pool.cpp: unit tests for the fast book's order pool.
 *
 * What it does: Checks the free list in every situation the fast book will hit: slots handed out
 *   in order at the start, every slot taken, a full pool, releasing and retaking, the slot
 *   released last coming back first, a one-slot pool, and a double release caught in Debug builds.
 *
 * Why it exists: The pool is the fast book's only memory for orders. If it hands out a slot twice,
 *   two orders overwrite each other, so it is tested on its own before any book uses it.
 *
 * How it works: After every step, free_list_consistent() re-walks the free list and checks it
 *   holds exactly the slots not in use.
 */

#include <gtest/gtest.h>
#include "nanobook/fast/order_pool.hpp"

#include <array>
#include <cstdint>
#include <set>

using namespace nanobook;
using namespace nanobook::fast;

TEST(OrderPool, StartsEmptyAndHandsOutSlotsInOrder) {
    OrderPool pool{4};
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.used(), 0U);
    EXPECT_TRUE(pool.free_list_consistent());
    // At the start the free list runs 0, 1, 2, ...: the first orders sit next to each other.
    EXPECT_EQ(pool.acquire(), 0U);
    EXPECT_EQ(pool.acquire(), 1U);
    EXPECT_EQ(pool.acquire(), 2U);
    EXPECT_EQ(pool.used(), 3U);
    EXPECT_TRUE(pool.free_list_consistent());
}

TEST(OrderPool, AcquiredSlotIsResetAndNotFree) {
    OrderPool pool{2};
    const std::uint32_t s = pool.acquire();
    pool[s].id = OrderId{7};
    pool[s].leaves = Qty{100};
    pool.release(s);
    const std::uint32_t again = pool.acquire();
    ASSERT_EQ(again, s);
    EXPECT_EQ(pool[again].id, OrderId{}); // old contents gone
    EXPECT_EQ(pool[again].leaves, Qty{});
    EXPECT_EQ(pool[again].prev, kNone); // not in any queue
    EXPECT_EQ(pool[again].next, kNone);
    EXPECT_FALSE(pool.is_free(again));
}

TEST(OrderPool, FullPoolReturnsNone) {
    OrderPool pool{3};
    std::set<std::uint32_t> taken;
    for (int i = 0; i < 3; ++i) {
        taken.insert(pool.acquire());
    }
    EXPECT_EQ(taken.size(), 3U); // three different slots
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.acquire(), kNone); // the book turns this into BookFull
    EXPECT_EQ(pool.used(), 3U);
    EXPECT_TRUE(pool.free_list_consistent());
}

TEST(OrderPool, LastReleasedIsReusedFirst) {
    OrderPool pool{8};
    const std::uint32_t a = pool.acquire();
    const std::uint32_t b = pool.acquire();
    const std::uint32_t c = pool.acquire();
    pool.release(a);
    pool.release(c);
    EXPECT_EQ(pool.acquire(), c); // most recently released: most likely still in cache
    EXPECT_EQ(pool.acquire(), a);
    EXPECT_FALSE(pool.is_free(b));
    EXPECT_TRUE(pool.free_list_consistent());
}

TEST(OrderPool, ReleaseFromFullThenRetake) {
    OrderPool pool{2};
    const std::uint32_t a = pool.acquire();
    (void)pool.acquire();
    ASSERT_TRUE(pool.full());
    pool.release(a);
    EXPECT_FALSE(pool.full());
    EXPECT_EQ(pool.acquire(), a);
    EXPECT_TRUE(pool.full());
    EXPECT_TRUE(pool.free_list_consistent());
}

TEST(OrderPool, OneSlotPool) {
    OrderPool pool{1};
    const std::uint32_t s = pool.acquire();
    EXPECT_EQ(s, 0U);
    EXPECT_EQ(pool.acquire(), kNone);
    pool.release(s);
    EXPECT_EQ(pool.acquire(), 0U);
}

TEST(OrderPool, ManyCyclesStayConsistent) {
    OrderPool pool{16};
    std::array<std::uint32_t, 16> held{};
    for (int round = 0; round < 100; ++round) {
        for (std::uint32_t &h : held) {
            h = pool.acquire();
            ASSERT_NE(h, kNone);
        }
        ASSERT_TRUE(pool.full());
        for (std::size_t i = 1; i < held.size(); i += 2) { // every other slot first, then the rest
            pool.release(held[i]);
        }
        for (std::size_t i = 0; i < held.size(); i += 2) {
            pool.release(held[i]);
        }
        ASSERT_EQ(pool.used(), 0U);
        ASSERT_TRUE(pool.free_list_consistent());
    }
}

TEST(OrderPoolDeathTest, DoubleReleaseIsCaughtInDebug) {
#ifdef NDEBUG
    GTEST_SKIP() << "asserts are compiled out in Release builds";
#else
    OrderPool pool{2};
    const std::uint32_t s = pool.acquire();
    pool.release(s);
    EXPECT_DEATH(pool.release(s), "released twice");
#endif
}

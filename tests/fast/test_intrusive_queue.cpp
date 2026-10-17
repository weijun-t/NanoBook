/*
 * test_intrusive_queue.cpp: unit tests for a price level's intrusive FIFO queue.
 *
 * What it does: Checks every way the fast book changes a queue: pushing onto an empty queue,
 *   keeping arrival order, removing the only order, the front, the back and the middle, popping
 *   until empty, moving an order to the back (a modify that loses priority), and two queues
 *   sharing one pool without interfering.
 *
 * Why it exists: A broken link silently loses orders or puts them in the wrong order, which breaks
 *   price-time priority. Each case is tested here on its own, before the book uses the queue.
 *
 * How it works: order_of() walks the queue front to back and returns the slots in order. After
 *   every change, links_consistent() re-checks that every link agrees in both directions.
 */

#include <gtest/gtest.h>
#include "nanobook/fast/intrusive_queue.hpp"
#include "nanobook/fast/order_pool.hpp"

#include <cstdint>
#include <vector>

using namespace nanobook::fast;

namespace {

std::vector<std::uint32_t> order_of(const IntrusiveQueue &q, const OrderPool &pool) {
    std::vector<std::uint32_t> slots;
    for (std::uint32_t s = q.front(); s != kNone; s = pool[s].next) {
        slots.push_back(s);
    }
    return slots;
}

class QueueTest : public ::testing::Test {
protected:
    OrderPool pool{16};
    IntrusiveQueue q;
    std::uint32_t a = pool.acquire();
    std::uint32_t b = pool.acquire();
    std::uint32_t c = pool.acquire();

    void push_abc() {
        q.push_back(pool, a);
        q.push_back(pool, b);
        q.push_back(pool, c);
    }
};

} // namespace

TEST_F(QueueTest, StartsEmpty) {
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.front(), kNone);
    EXPECT_EQ(q.back(), kNone);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, PushOntoEmptyQueue) {
    q.push_back(pool, a);
    EXPECT_EQ(q.front(), a);
    EXPECT_EQ(q.back(), a);
    EXPECT_EQ(q.size(), 1U);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, KeepsArrivalOrder) {
    push_abc();
    EXPECT_EQ(order_of(q, pool), (std::vector{a, b, c}));
    EXPECT_EQ(q.front(), a); // oldest trades first
    EXPECT_EQ(q.back(), c);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, UnlinkTheOnlyOrder) {
    q.push_back(pool, a);
    q.unlink(pool, a);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.front(), kNone);
    EXPECT_EQ(q.back(), kNone);
    EXPECT_EQ(pool[a].prev, kNone); // links reset: ready to queue again or release
    EXPECT_EQ(pool[a].next, kNone);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, UnlinkTheFront) {
    push_abc();
    q.unlink(pool, a);
    EXPECT_EQ(order_of(q, pool), (std::vector{b, c}));
    EXPECT_EQ(q.front(), b);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, UnlinkTheBack) {
    push_abc();
    q.unlink(pool, c);
    EXPECT_EQ(order_of(q, pool), (std::vector{a, b}));
    EXPECT_EQ(q.back(), b);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, UnlinkTheMiddle) {
    push_abc();
    q.unlink(pool, b); // a cancel from the middle of the queue
    EXPECT_EQ(order_of(q, pool), (std::vector{a, c}));
    EXPECT_EQ(pool[a].next, c); // the neighbours now point at each other
    EXPECT_EQ(pool[c].prev, a);
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, PopFrontUntilEmpty) {
    push_abc();
    std::vector<std::uint32_t> popped;
    while (!q.empty()) {
        const std::uint32_t s = q.front(); // how matching consumes a level
        q.unlink(pool, s);
        popped.push_back(s);
        ASSERT_TRUE(q.links_consistent(pool));
    }
    EXPECT_EQ(popped, (std::vector{a, b, c}));
    EXPECT_EQ(q.back(), kNone);
}

TEST_F(QueueTest, MoveToBackLosesPriority) {
    push_abc();
    q.unlink(pool, a); // a modify that grows the order: same price, back of the queue
    q.push_back(pool, a);
    EXPECT_EQ(order_of(q, pool), (std::vector{b, c, a}));
    EXPECT_TRUE(q.links_consistent(pool));
}

TEST_F(QueueTest, UnlinkThenReleaseThenReuseTheSlot) {
    push_abc();
    q.unlink(pool, b);
    pool.release(b);
    const std::uint32_t d = pool.acquire(); // gets b's slot back (last released, first reused)
    EXPECT_EQ(d, b);
    q.push_back(pool, d);
    EXPECT_EQ(order_of(q, pool), (std::vector{a, c, d}));
    EXPECT_TRUE(q.links_consistent(pool));
    EXPECT_TRUE(pool.free_list_consistent());
}

TEST_F(QueueTest, TwoQueuesShareOnePool) {
    IntrusiveQueue other; // e.g. another price level
    q.push_back(pool, a);
    other.push_back(pool, b);
    q.push_back(pool, c);
    EXPECT_EQ(order_of(q, pool), (std::vector{a, c}));
    EXPECT_EQ(order_of(other, pool), (std::vector{b}));
    other.unlink(pool, b);
    EXPECT_EQ(order_of(q, pool), (std::vector{a, c})); // untouched
    EXPECT_TRUE(q.links_consistent(pool));
    EXPECT_TRUE(other.links_consistent(pool));
}

TEST_F(QueueTest, LinksConsistentCatchesABrokenLink) {
    push_abc();
    pool[c].prev = a; // corrupt a back link on purpose: c should point back at b
    EXPECT_FALSE(q.links_consistent(pool));
}

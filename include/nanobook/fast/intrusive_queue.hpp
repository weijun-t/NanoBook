/*
 * intrusive_queue.hpp: one price level's first-come-first-served queue of orders.
 *
 * What it does: Keeps the orders waiting at one price in arrival order. push_back() adds an order
 *   at the back (a new order, or a modify that loses priority); unlink() removes one from anywhere
 *   (a cancel, a fill, a self-match cancel); front() is the oldest order, the next to trade.
 *
 * Why it exists: The reference book's std::list wraps every order in its own heap-allocated node.
 *   Here nothing is allocated: the orders already live in the pool, and the queue only links them.
 *
 * How it works: A doubly linked list whose links live inside the orders themselves ("intrusive"):
 *   each Order has `prev` and `next` slot numbers, and the queue only remembers its first slot,
 *   its last slot and how many orders it holds. Removing from the middle connects the neighbours
 *   to each other. Every operation is O(1).
 *
 * Data structures: Three uint32_t (head, tail, count): 12 bytes. The orders are in the OrderPool.
 *
 * Trade-offs: The queue can't check on its own that an order belongs to it, so unlinking an order
 *   from the wrong queue would corrupt both. The book's invariant check (links_consistent) and the
 *   differential fuzzer are what catch that.
 */

#pragma once

#include <cassert>
#include <cstdint>

#include "nanobook/fast/order.hpp"
#include "nanobook/fast/order_pool.hpp"

namespace nanobook::fast {

class IntrusiveQueue {
public:
    // Adds `slot` at the back: the newest, the last to trade. It must not be in any queue.
    void push_back(OrderPool &pool, std::uint32_t slot) {
        Order &order = pool[slot];
        assert(order.prev == kNone && order.next == kNone && "order is already in a queue");
        order.prev = tail_;
        if (tail_ == kNone) {
            head_ = slot; // the queue was empty
        } else {
            pool[tail_].next = slot;
        }
        tail_ = slot;
        ++count_;
    }

    // Removes `slot` from wherever it is in this queue. Its links are reset, so it can be pushed
    // into a queue again (a modify that loses priority) or released to the pool.
    void unlink(OrderPool &pool, std::uint32_t slot) {
        assert(count_ > 0);
        Order &order = pool[slot];
        if (order.prev == kNone) {
            head_ = order.next; // it was the front
        } else {
            pool[order.prev].next = order.next;
        }
        if (order.next == kNone) {
            tail_ = order.prev; // it was the back
        } else {
            pool[order.next].prev = order.prev;
        }
        order.prev = kNone;
        order.next = kNone;
        --count_;
    }

    [[nodiscard]] std::uint32_t front() const {
        return head_; // kNone if empty
    }
    [[nodiscard]] std::uint32_t back() const {
        return tail_;
    }
    [[nodiscard]] std::uint32_t size() const {
        return count_;
    }
    [[nodiscard]] bool empty() const {
        return count_ == 0;
    }

    // Walks the queue front to back and checks that every link agrees in both directions and the
    // count is right (for invariant checks; O(size), tests only).
    [[nodiscard]] bool links_consistent(const OrderPool &pool) const {
        std::uint32_t seen = 0;
        std::uint32_t previous = kNone;
        for (std::uint32_t s = head_; s != kNone; s = pool[s].next) {
            if (s >= pool.capacity() || pool.is_free(s) || pool[s].prev != previous ||
                ++seen > count_) {
                return false; // out of range, a free slot, a broken back link, or a loop
            }
            previous = s;
        }
        return seen == count_ && previous == tail_;
    }

private:
    std::uint32_t head_{kNone}; // oldest order
    std::uint32_t tail_{kNone}; // newest order
    std::uint32_t count_{0};
};

static_assert(sizeof(IntrusiveQueue) == 12);

} // namespace nanobook::fast

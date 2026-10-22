/*
 * order_pool.hpp: all of the fast book's order slots, allocated once, with a free list.
 *
 * What it does: Holds exactly `capacity` Order slots. acquire() hands out a free slot number (or
 *   kNone when every slot is in use, which the book reports as BookFull); release() gives one back.
 *
 * Why it exists: The reference book allocates memory for every order (about 2 allocations per
 *   added order). Rule 1 says the fast book must allocate nothing while trading. Like a car park
 *   with numbered spaces: all the spaces are built once, and orders just take and return them.
 *
 * How it works: The free slots form a singly linked list through their own `next` field, so it
 *   needs no extra memory. acquire() takes the head of the list; release() puts the slot back at
 *   the head. Both are O(1): a few instructions. The slot released last is reused first, because
 *   it is the one most likely to still be in the CPU cache. At the start the list runs 0, 1, 2,
 * ..., so the first orders sit next to each other in memory.
 *
 * Data structures: std::pmr::vector<Order>, sized once in the constructor and never resized, plus
 *   the free-list head and a count of slots in use. pmr: the memory comes from whatever memory
 *   resource is the default when the pool is built: plain new/delete normally, a huge-page arena
 *   in the timed tools (optimise design E6). arena_bytes() says how much that is.
 *
 * Trade-offs: Which slot an order gets never appears in any event, so slot reuse can't change the
 *   output (rule 11). Releasing a free slot twice is a bug: caught by assert in Debug builds, using
 *   the kFreeTag stored in free slots' `prev`.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "nanobook/fast/order.hpp"

namespace nanobook::fast {

class OrderPool {
public:
    explicit OrderPool(std::uint32_t capacity) : slots_(capacity) {
        assert(capacity < kFreeTag && "slot numbers kNone and kFreeTag are reserved");
        for (std::uint32_t i = 0; i < capacity; ++i) { // free list: 0 -> 1 -> 2 -> ... -> none
            slots_[i].prev = kFreeTag;
            slots_[i].next = i + 1 < capacity ? i + 1 : kNone;
        }
        free_head_ = capacity > 0 ? 0 : kNone;
    }

    // The bytes a pool of `capacity` slots takes from its memory resource.
    [[nodiscard]] static std::size_t arena_bytes(std::uint32_t capacity) {
        return std::size_t{capacity} * sizeof(Order);
    }

    // A free slot, reset and ready to fill in, or kNone if every slot is in use.
    [[nodiscard]] std::uint32_t acquire() {
        const std::uint32_t slot = free_head_;
        if (slot == kNone) {
            return kNone;
        }
        free_head_ = slots_[slot].next;
        slots_[slot] = Order{}; // prev and next back to kNone: not in any queue yet
        ++used_;
        return slot;
    }

    // Gives a slot back. It must be in use and no longer linked into any queue.
    void release(std::uint32_t slot) {
        assert(slot < slots_.size());
        assert(!is_free(slot) && "slot released twice");
        slots_[slot].prev = kFreeTag;
        slots_[slot].next = free_head_;
        free_head_ = slot;
        --used_;
    }

    [[nodiscard]] Order &operator[](std::uint32_t slot) {
        assert(slot < slots_.size());
        return slots_[slot];
    }
    [[nodiscard]] const Order &operator[](std::uint32_t slot) const {
        assert(slot < slots_.size());
        return slots_[slot];
    }

    [[nodiscard]] bool is_free(std::uint32_t slot) const {
        return slots_[slot].prev == kFreeTag;
    }
    [[nodiscard]] std::uint32_t capacity() const {
        return static_cast<std::uint32_t>(slots_.size());
    }
    [[nodiscard]] std::uint32_t used() const {
        return used_;
    }
    [[nodiscard]] bool full() const {
        return free_head_ == kNone;
    }

    // Walks the free list and checks it holds exactly the slots not in use (for invariant checks;
    // O(capacity), tests only).
    [[nodiscard]] bool free_list_consistent() const {
        std::uint32_t count = 0;
        for (std::uint32_t s = free_head_; s != kNone; s = slots_[s].next) {
            if (s >= slots_.size() || !is_free(s) || ++count > slots_.size()) {
                return false; // out of range, not marked free, or a loop
            }
        }
        return count == capacity() - used_;
    }

private:
    std::pmr::vector<Order> slots_; // sized once; never resized
    std::uint32_t free_head_{kNone};
    std::uint32_t used_{0};
};

} // namespace nanobook::fast

/*
 * spsc_ring.hpp: a lock-free ring buffer for exactly one producer thread and one consumer thread.
 *
 * What it does: SpscRing<T, Capacity> passes records of type T from one thread to another.
 *   try_push() adds one at the head (false if the ring is full), try_pop() takes one from the tail
 *   (false if it is empty), in the order they were pushed. push() and pop() wait instead of
 *   failing, and return how many times they found the ring full or empty.
 *
 * Why it exists: The pipeline's three threads hand records to each other (pipeline design §2).
 *   A mutex would cost tens of nanoseconds per hand-over and can put a thread to sleep (a system
 *   call, rule 6). With one writer and one reader, no lock is needed: each side writes only its
 *   own counter (README rule 5).
 *
 * How it works (pipeline design §3):
 *   - head_ counts records pushed, tail_ records popped. Both only increase; a record's slot is
 *     counter & (Capacity - 1), so Capacity is a power of two. Empty: head == tail. Full:
 *     head - tail == Capacity.
 *   - Producer: write the slot, then head_.store(release). Consumer: head_.load(acquire), then
 *     read the slot. The release/acquire pair makes the record's bytes visible before the new
 *     head_ is. Symmetrically, the consumer's tail_.store(release) after reading and the
 *     producer's tail_.load(acquire) before overwriting stop a slot being reused while it is
 *     still being read. Each side reads its own counter with relaxed: only it writes it.
 *   - Cached counters: the producer keeps the last tail_ it saw and reloads the real one only when
 *     the ring looks full; the consumer does the same with head_. Most pushes and pops then touch
 *     only their own cache lines.
 *   - Cost: O(1) per push and pop, one copy of T, no allocation.
 *
 * Data structures: head_, cached_tail_, tail_ and cached_head_ each on their own 128-byte block
 *   (rule 9: Intel CPUs prefetch the adjacent 64-byte line too), so the two cores never fight
 *   over one line (false sharing). The slots are a std::vector sized and zero-filled once in the
 *   constructor: every page is touched up front, so none is first touched mid-run (phase 4's
 *   page-fault lesson).
 *
 * Trade-offs: Exactly one producer and one consumer thread: a second of either is a data race.
 *   Waiting spins with the CPU's pause instruction instead of sleeping, so each side should own a
 *   core. T must be trivially copyable (records are copied as bytes).
 */

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace nanobook::pipeline {

// Rule 9: two 64-byte lines, because Intel CPUs prefetch the adjacent line in pairs.
inline constexpr std::size_t kNoFalseSharing = 128;

// Tells the CPU this is a spin-wait loop: it saves power and avoids a costly pipeline flush when
// the awaited value changes. Not a system call; the thread keeps its core.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

template <typename T, std::size_t Capacity> class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "records are copied as bytes");
    static_assert(Capacity >= 2 && std::has_single_bit(Capacity),
                  "Capacity must be a power of two: a slot is counter & (Capacity - 1)");

public:
    SpscRing() : slots_(Capacity) {
    }
    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;
    SpscRing(SpscRing &&) = delete;
    SpscRing &operator=(SpscRing &&) = delete;
    ~SpscRing() = default;

    // Producer thread only. False if the ring is full.
    [[nodiscard]] bool try_push(const T &item) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed); // only we write it
        if (head - cached_tail_ == Capacity) {                            // looks full
            cached_tail_ = tail_.load(std::memory_order_acquire); // pairs with the pop's release
            if (head - cached_tail_ == Capacity) {
                return false;
            }
        }
        slots_[head & kMask] = item;
        head_.store(head + 1, std::memory_order_release); // publishes the slot written above
        return true;
    }

    // Consumer thread only. False if the ring is empty.
    [[nodiscard]] bool try_pop(T &out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed); // only we write it
        if (tail == cached_head_) {                                       // looks empty
            cached_head_ = head_.load(std::memory_order_acquire); // pairs with the push's release
            if (tail == cached_head_) {
                return false;
            }
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release); // the slot is free to reuse
        return true;
    }

    // Waits until there is room. Returns how many times the ring was found full (0 normally).
    std::uint64_t push(const T &item) noexcept {
        std::uint64_t waits = 0;
        while (!try_push(item)) {
            ++waits;
            cpu_relax();
        }
        return waits;
    }

    // Waits until there is a record. Returns how many times the ring was found empty.
    std::uint64_t pop(T &out) noexcept {
        std::uint64_t waits = 0;
        while (!try_pop(out)) {
            ++waits;
            cpu_relax();
        }
        return waits;
    }

    [[nodiscard]] static constexpr std::size_t capacity() {
        return Capacity;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    alignas(kNoFalseSharing) std::atomic<std::uint64_t> head_{0}; // written by the producer
    alignas(kNoFalseSharing) std::uint64_t cached_tail_{0};       // the producer's copy of tail_
    alignas(kNoFalseSharing) std::atomic<std::uint64_t> tail_{0}; // written by the consumer
    alignas(kNoFalseSharing) std::uint64_t cached_head_{0};       // the consumer's copy of head_
    alignas(kNoFalseSharing) std::vector<T> slots_;               // sized and touched once
};

} // namespace nanobook::pipeline

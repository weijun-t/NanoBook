/*
 * test_spsc_ring.cpp: tests for the lock-free ring buffer
 * (include/nanobook/pipeline/spsc_ring.hpp).
 *
 * What it does: One thread first: a new ring is empty; records come out in the order they went
 *   in; a full ring refuses a push until something is popped; the counters wrap round the array
 *   thousands of times without losing a record. Then two threads, the ring's real use: a producer
 *   pushes 1, 2, 3, ... and the consumer checks that every number arrives exactly once and in
 *   order, on a big ring and on a 2-slot ring (constantly full and empty, so the two threads race
 *   on every record); and 64-byte records whose eight fields are all the same number, so a record
 *   read half-written (the bug wrong memory orders cause) shows up as fields that disagree.
 *
 * Why it exists: A ring that loses, repeats or tears a record makes every later stage wrong
 *   without crashing. The two-thread tests run under ThreadSanitizer in CI (pipeline design §8),
 *   which also reports any access the memory orders don't make safe.
 *
 * How it works: std::jthread runs the producer while the test body is the consumer. Under TSan
 *   (5-15x slower) the stress tests use fewer records.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "nanobook/pipeline/spsc_ring.hpp"

using nanobook::pipeline::SpscRing;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true; // GCC
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true; // Clang
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

constexpr std::uint64_t kStress = kThreadSanitizer ? 200'000 : 10'000'000;

// Eight copies of one number: if they ever disagree, a record was read half-written.
struct Wide {
    std::array<std::uint64_t, 8> v{};
};
static_assert(sizeof(Wide) == 64);

// Producer pushes 1..n; the calling thread pops n and checks each one. Returns the consumer's
// count of empty waits (only to show the threads really overlapped).
template <std::size_t Capacity> std::uint64_t one_to_n_in_order(std::uint64_t n) {
    auto ring = std::make_unique<SpscRing<std::uint64_t, Capacity>>();
    std::jthread producer{[&ring, n] {
        for (std::uint64_t i = 1; i <= n; ++i) {
            ring->push(i);
        }
    }};
    std::uint64_t waits = 0;
    for (std::uint64_t expected = 1; expected <= n; ++expected) {
        std::uint64_t got = 0;
        waits += ring->pop(got);
        if (got != expected) {
            ADD_FAILURE() << "expected " << expected << ", got " << got;
            break; // the producer still finishes: the ring never blocks it for long
        }
    }
    producer.join();
    std::uint64_t extra = 0;
    EXPECT_FALSE(ring->try_pop(extra)) << "a record arrived twice";
    return waits;
}

} // namespace

// ---------------------------------------------------------------------------
// One thread
// ---------------------------------------------------------------------------

TEST(SpscRing, StartsEmpty) {
    SpscRing<int, 8> ring;
    int out = 0;
    EXPECT_FALSE(ring.try_pop(out));
    EXPECT_EQ((SpscRing<int, 8>::capacity()), 8U); // parentheses: the comma would split the macro
}

TEST(SpscRing, RecordsComeOutInTheOrderTheyWentIn) {
    SpscRing<int, 8> ring;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.try_push(i * 10));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i * 10);
    }
    int out = 0;
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, AFullRingRefusesUntilOneIsPopped) {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(99)); // full: head - tail == 4
    int out = -1;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(ring.try_push(4)); // one slot free again
    EXPECT_FALSE(ring.try_push(5));
    for (int expected = 1; expected <= 4; ++expected) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, expected);
    }
}

TEST(SpscRing, WrapsRoundTheArrayWithoutLosingARecord) {
    SpscRing<std::uint64_t, 4> ring;
    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    for (int round = 0; round < 10'000; ++round) { // pushes 1-4, pops 1-4: every fill level
        const int burst = 1 + round % 4;
        for (int i = 0; i < burst; ++i) {
            ASSERT_TRUE(ring.try_push(next_in++));
        }
        for (int i = 0; i < burst; ++i) {
            std::uint64_t out = 0;
            ASSERT_TRUE(ring.try_pop(out));
            ASSERT_EQ(out, next_out++);
        }
    }
    EXPECT_GT(next_in, 4U * 1000); // the counters went round the 4 slots thousands of times
}

TEST(SpscRing, PushAndPopReportTheirWaits) {
    SpscRing<int, 2> ring;
    EXPECT_EQ(ring.push(1), 0U); // room: no wait
    int out = 0;
    EXPECT_EQ(ring.pop(out), 0U); // a record: no wait
    EXPECT_EQ(out, 1);
}

// ---------------------------------------------------------------------------
// Two threads (run under ThreadSanitizer in CI)
// ---------------------------------------------------------------------------

TEST(SpscRingThreads, EveryRecordArrivesOnceAndInOrder) {
    one_to_n_in_order<65'536>(kStress);
}

TEST(SpscRingThreads, ATwoSlotRingRacesOnEveryRecord) {
    // Full after 2 pushes, empty after 2 pops: the producer and consumer wait on each other
    // almost every time, so both cached counters are reloaded constantly.
    one_to_n_in_order<2>(kStress / 10);
}

TEST(SpscRingThreads, NoRecordIsEverReadHalfWritten) {
    auto ring = std::make_unique<SpscRing<Wide, 16>>();
    constexpr std::uint64_t n = kStress / 10; // a constant: the lambda needn't capture it
    std::jthread producer{[&ring] {
        for (std::uint64_t i = 1; i <= n; ++i) {
            Wide w;
            w.v.fill(i);
            ring->push(w);
        }
    }};
    for (std::uint64_t expected = 1; expected <= n; ++expected) {
        Wide w;
        ring->pop(w);
        for (const std::uint64_t field : w.v) {
            if (field != expected) {
                ADD_FAILURE() << "record " << expected << " has a field " << field;
                return; // the jthread joins on the way out
            }
        }
    }
}

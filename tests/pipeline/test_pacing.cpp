/*
 * test_pacing.cpp: tests for the paced feed's stamp (support/pacing.hpp).
 *
 * What it does: With a fake clock that moves one tick each time it is read: records before the
 *   chosen ITCH time go out at once and unstamped; from that time on, record n is stamped
 *   start + n x (ticks per second / rate), whatever the moment it was asked for; the pacer waits
 *   until the clock reaches that time; and a feed that falls behind is still stamped with the
 *   intended times, so its delay counts (coordinated omission, rule 18).
 *
 * Why it exists: Step 4's latency numbers are only as honest as these stamps.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "nanobook/pipeline/records.hpp"
#include "pacing.hpp"

using namespace nanobook::pipeline;

namespace {

// Moves one tick every time it is read, starting at *now.
struct FakeClock {
    std::uint64_t *now;
    std::uint64_t operator()() const {
        return (*now)++;
    }
};

FeedRecord at(std::uint64_t itch_ns) {
    return FeedRecord{.timestamp = itch_ns, .kind = FeedKind::Add};
}

} // namespace

TEST(Pacer, TheWarmUpIsNeitherPacedNorStamped) {
    std::uint64_t now = 1'000;
    Pacer pacer{FakeClock{&now}, 1'000, 10, 500}; // 1,000 ticks a second, 10 records a second
    EXPECT_EQ(pacer(at(100)), 0U);
    EXPECT_EQ(pacer(at(499)), 0U);
    EXPECT_EQ(now, 1'000U); // the clock wasn't even read
    EXPECT_EQ(pacer.paced(), 0U);
}

TEST(Pacer, StampsTheIntendedTimesAndWaitsForThem) {
    std::uint64_t now = 1'000;
    Pacer pacer{FakeClock{&now}, 1'000, 10, 500}; // one record every 100 ticks
    EXPECT_EQ(pacer(at(500)), 1'000U);            // the first paced record: start = 1,000
    EXPECT_EQ(pacer(at(600)), 1'100U);
    EXPECT_GE(now, 1'100U); // it waited until the intended time
    EXPECT_EQ(pacer(at(700)), 1'200U);
    EXPECT_EQ(pacer.paced(), 3U);
    // Records after the start are paced even if their ITCH time is earlier (S, R, H have times).
    EXPECT_EQ(pacer(at(10)), 1'300U);
}

TEST(Pacer, AFeedThatFallsBehindIsStillStampedOnSchedule) {
    std::uint64_t now = 1'000;
    Pacer pacer{FakeClock{&now}, 1'000, 10, 0};
    EXPECT_EQ(pacer(at(0)), 1'000U);
    now += 5'000;                    // the feed stalls for 5,000 ticks (50 records' worth)
    EXPECT_EQ(pacer(at(1)), 1'100U); // stamped when it should have gone, not when it did:
    EXPECT_EQ(pacer(at(2)), 1'200U); // the stall shows up as latency downstream
    EXPECT_GT(now, 6'000U);          // and nothing waited: those times are long past
}

TEST(Pacer, TheFirstPacedRecordWaitsForTheSettleTime) {
    std::uint64_t now = 1'000;
    Pacer pacer{FakeClock{&now}, 1'000, 10, 500, 2'000}; // settle 2,000 ticks after the warm-up
    EXPECT_EQ(pacer(at(100)), 0U);                       // warm-up
    EXPECT_EQ(pacer(at(500)), 3'000U);                   // 1,000 + 2,000: the backlog drains
    EXPECT_GE(now, 3'000U);
    EXPECT_EQ(pacer(at(600)), 3'100U);
}

TEST(Pacer, WholeNumberArithmeticDoesNotDrift) {
    std::uint64_t now = 1;
    // 2.4 billion ticks a second, 3 million records a second: 800 ticks each, exactly.
    Pacer pacer{FakeClock{&now}, 2'400'000'000, 3'000'000, 0};
    std::uint64_t last = 0;
    for (int i = 0; i < 1'000; ++i) {
        last = pacer(at(0));
    }
    EXPECT_EQ(last, 1 + 999U * 800); // record 999, no rounding error carried along
}

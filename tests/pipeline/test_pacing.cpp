/*
 * test_pacing.cpp: tests for the paced feed's stamp (support/pacing.hpp).
 *
 * What it does: With a fake clock that moves one tick each time it is read: records before the
 *   chosen ITCH time go out at once and unstamped; from that time on, record n is stamped
 *   start + n x (ticks per second / rate), whatever the moment it was asked for; the pacer waits
 *   until the clock reaches that time; and a feed that falls behind is still stamped with the
 *   intended times, so its delay counts (coordinated omission, rule 18). ClockPacer (viewer
 *   design §5) the same way on the ITCH clock: at 1x a record 1 ms of ITCH time later is due 1 ms
 *   of ticks later, at 10x a tenth of that; an earlier ITCH time never moves the schedule back;
 *   and a whole day at a 5 GHz counter gives the exact tick count (no overflow, no drift).
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

TEST(ClockPacer, TheWarmUpIsNeitherPacedNorStamped) {
    std::uint64_t now = 1'000;
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 1, 500};
    EXPECT_EQ(pacer(at(100)), 0U);
    EXPECT_EQ(pacer(at(499)), 0U);
    EXPECT_EQ(now, 1'000U); // the clock wasn't even read
    EXPECT_EQ(pacer.paced(), 0U);
}

TEST(ClockPacer, FollowsTheItchClockAtOneTimesSpeed) {
    std::uint64_t now = 1'000;
    // 1 million ticks a second: 1 ms of ITCH time (10^6 ns) is 1,000 ticks.
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 1, 0};
    EXPECT_EQ(pacer(at(0)), 1'000U);             // the first paced record: start = 1,000
    EXPECT_EQ(pacer(at(1'000'000)), 2'000U);     // 1 ms later
    EXPECT_GE(now, 2'000U);                      // it waited until then
    EXPECT_EQ(pacer(at(1'000'000)), 2'000U);     // same ITCH time: due at once
    EXPECT_EQ(pacer(at(250'000'000)), 251'000U); // 250 ms later
    EXPECT_EQ(pacer.paced(), 4U);
}

TEST(ClockPacer, TenTimesSpeedTakesATenthOfTheTime) {
    std::uint64_t now = 1'000;
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 10, 0};
    EXPECT_EQ(pacer(at(0)), 1'000U);
    EXPECT_EQ(pacer(at(1'000'000)), 1'100U); // 1 ms of ITCH time in 0.1 ms
    EXPECT_EQ(pacer(at(10'000'000)), 2'000U);
}

TEST(ClockPacer, AnEarlierItchTimeNeverMovesTheScheduleBack) {
    std::uint64_t now = 1'000;
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 1, 0};
    EXPECT_EQ(pacer(at(5'000'000)), 6'000U);
    EXPECT_EQ(pacer(at(2'000'000)), 6'000U); // due at the last one's time, not 3,000
    EXPECT_EQ(pacer(at(6'000'000)), 7'000U);
}

TEST(ClockPacer, TheFirstPacedRecordWaitsForTheSettleTime) {
    std::uint64_t now = 1'000;
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 1, 500, 2'000};
    EXPECT_EQ(pacer(at(100)), 0U);     // warm-up
    EXPECT_EQ(pacer(at(500)), 3'000U); // 1,000 + 2,000
    EXPECT_GE(now, 3'000U);
}

TEST(ClockPacer, AWholeDayAtFiveGigahertzIsExact) {
    std::uint64_t now = 0;
    // 04:00 to 20:00 is 57,600 s; at 5 * 10^9 ticks a second that is 2.88 * 10^14 ticks. Done
    // naively, 5.76 * 10^13 ns x 5 * 10^9 would overflow 2^64 (1.8 * 10^19).
    constexpr std::uint64_t kFrom = 4ULL * 3'600 * 1'000'000'000;
    constexpr std::uint64_t kTo = 20ULL * 3'600 * 1'000'000'000;
    ClockPacer pacer{FakeClock{&now}, 5'000'000'000, 1, kFrom};
    EXPECT_EQ(pacer(at(kFrom)), 0U); // start = 0: the fake clock was at 0
    now = 288'000'000'000'000;       // jump ahead: no 2.88 * 10^14-iteration wait
    EXPECT_EQ(pacer(at(kTo)), 288'000'000'000'000U);
    EXPECT_EQ(pacer(at(kTo - 1)), 288'000'000'000'000U); // and 1 ns earlier never goes back
}

TEST(ClockPacer, AFeedThatFallsBehindIsStillStampedOnSchedule) {
    std::uint64_t now = 1'000;
    ClockPacer pacer{FakeClock{&now}, 1'000'000, 1, 0};
    EXPECT_EQ(pacer(at(0)), 1'000U);
    now += 50'000;                           // the feed stalls for 50 ms
    EXPECT_EQ(pacer(at(1'000'000)), 2'000U); // stamped when it was due, not when it went
}

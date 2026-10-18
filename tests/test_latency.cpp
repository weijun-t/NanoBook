/*
 * test_latency.cpp: tests for the latency harness (phase 3).
 *
 * What it does: Checks the timer (support/tsc.hpp): reading the CPU flags, converting ticks to
 *   nanoseconds, that counter readings only go forward, that calibration gives a believable and
 *   stable tick rate, and that the timer's own cost is measured sensibly.
 *
 * Why it exists: A wrong harness gives confident wrong numbers (latency-harness design §10), so
 *   its parts are tested like the engine's.
 *
 * Trade-offs: Tests that read the real counter can only check what holds on every machine
 *   (forward readings, a rate between 0.1 and 10 ticks per ns, a small spread). Exact values
 *   depend on the CPU, so those are checked with made-up inputs instead.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "tsc.hpp"

using namespace nanobook::latency;

TEST(TscFlags, MatchesWholeWordsOnly) {
    constexpr std::string_view kFlags = " fpu constant_tsc rdtscp nonstop_tsc tsc";
    EXPECT_TRUE(has_flag(kFlags, "fpu"));       // first word (after a leading space)
    EXPECT_TRUE(has_flag(kFlags, "tsc"));       // last word
    EXPECT_TRUE(has_flag(kFlags, "rdtscp"));    // middle
    EXPECT_FALSE(has_flag(kFlags, "constant")); // part of a word is not the word
    EXPECT_FALSE(has_flag(kFlags, "rdtsc"));    // a prefix of rdtscp is not rdtsc
    EXPECT_FALSE(has_flag("", "tsc"));
}

TEST(TscFlags, ReportsWhatIsMissing) {
    EXPECT_TRUE(missing_tsc_flags("constant_tsc nonstop_tsc rdtscp").empty());
    EXPECT_EQ(missing_tsc_flags("constant_tsc rdtscp"),
              std::vector<std::string_view>{"nonstop_tsc"});
    EXPECT_EQ(missing_tsc_flags("").size(), 3U);
}

TEST(TscFlags, ReadsThisMachinesFlags) {
    // Every x86-64 Linux machine lists at least the "tsc" flag.
    EXPECT_TRUE(has_flag(read_cpu_flags(), "tsc"));
}

TEST(Tsc, TicksToNanoseconds) {
    EXPECT_DOUBLE_EQ(to_ns(250, 2.5), 100.0); // design §4's worked example: 2.5 ticks per ns
    EXPECT_DOUBLE_EQ(to_ns(0, 2.5), 0.0);
    EXPECT_DOUBLE_EQ(to_ns(3, 1.5), 2.0);
}

TEST(Tsc, ReadingsOnlyGoForward) {
    Stamp previous = start();
    for (int i = 0; i < 100'000; ++i) {
        const Stamp next = stop();
        if (next.core == previous.core) { // different cores' counters are synchronised by the
            EXPECT_GT(next.ticks, previous.ticks); // OS, but only compare like with like
        }
        previous = next;
    }
}

TEST(Tsc, CalibrationIsBelievableAndStable) {
    const Calibration c = calibrate(3, 50'000'000); // 3 rounds of 50 ms
    // Real CPUs' counters tick between a few hundred MHz and a few GHz.
    EXPECT_GT(c.ticks_per_ns, 0.1);
    EXPECT_LT(c.ticks_per_ns, 10.0);
    // The rounds agree to within 1% (design §4 expects far better; this allows a busy CI machine).
    EXPECT_LT(c.spread, 0.01);
}

TEST(Tsc, OverheadIsMeasured) {
    const Overhead o = measure_overhead(10'000);
    EXPECT_GT(o.min, 0U); // two readings can never be the same tick
    EXPECT_LE(o.min, o.median);
    EXPECT_LE(o.median, o.max);
}

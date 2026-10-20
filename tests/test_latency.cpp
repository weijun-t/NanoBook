/*
 * test_latency.cpp: tests for the latency harness (phase 3).
 *
 * What it does: Checks the timer (support/tsc.hpp): reading the CPU flags, converting ticks to
 *   nanoseconds, that counter readings only go forward, that calibration gives a believable and
 *   stable tick rate, and that the timer's own cost is measured sensibly. Then the recorder
 *   (support/latency_recorder.hpp): samples kept in order with their tags, bad samples dropped or
 *   clamped and counted, nearest-rank percentiles on known data, overhead subtraction, the
 *   histogram's buckets, and a planted slow operation the tail percentiles must catch. Last, the
 *   environment parsers (support/environment.hpp) on made-up /proc and /sys contents, and when
 *   numbers may be published (bare metal and a clean commit only).
 *
 * Why it exists: A wrong harness gives confident wrong numbers (latency-harness design §10), so
 *   its parts are tested like the engine's. The planted slow operation is the harness's version of
 *   phase 2's planted bugs: proof that a slow tail can't hide from it.
 *
 * Trade-offs: Tests that read the real counter can only check what holds on every machine
 *   (forward readings, a rate between 0.1 and 10 ticks per ns, a small spread, a 1.2 µs stall
 *   seen as at least 1 µs). Exact values depend on the CPU, so those are checked with made-up
 *   inputs instead.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "environment.hpp"
#include "latency_recorder.hpp"
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

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

namespace {

// Two stamps `ticks` apart on core 0.
void record_ticks(Recorder &r, std::uint64_t ticks, Tag tag = Tag::Other) {
    r.record(Stamp{.ticks = 1'000, .core = 0}, Stamp{.ticks = 1'000 + ticks, .core = 0}, tag);
}

} // namespace

TEST(Recorder, KeepsSamplesInOrderWithTheirTags) {
    Recorder r{8};
    record_ticks(r, 30, Tag::Add);
    record_ticks(r, 10, Tag::Cancel);
    record_ticks(r, 20, Tag::Add);
    ASSERT_EQ(r.size(), 3U);
    EXPECT_EQ(r.ticks()[0], 30U); // recording order, not sorted
    EXPECT_EQ(r.ticks()[1], 10U);
    EXPECT_EQ(r.tags()[1], Tag::Cancel);
}

TEST(Recorder, DropsASampleTakenAcrossTwoCores) {
    Recorder r{8};
    r.record(Stamp{.ticks = 100, .core = 1}, Stamp{.ticks = 150, .core = 2}, Tag::Add);
    EXPECT_EQ(r.size(), 0U);
    EXPECT_EQ(r.dropped(), 1U);
}

TEST(Recorder, ClampsASampleTooLongFor32Bits) {
    Recorder r{8};
    record_ticks(r, std::uint64_t{1} << 40);
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r.ticks()[0], 0xFFFF'FFFFU);
    EXPECT_EQ(r.clamped(), 1U);
}

TEST(Recorder, RefusesSamplesPastItsCapacity) {
    Recorder r{2};
    record_ticks(r, 1);
    record_ticks(r, 2);
    record_ticks(r, 3);
    EXPECT_EQ(r.size(), 2U);
    EXPECT_EQ(r.refused(), 1U);
}

TEST(Recorder, ClearStartsOver) {
    Recorder r{2};
    record_ticks(r, 1);
    r.record(Stamp{.ticks = 0, .core = 1}, Stamp{.ticks = 5, .core = 2}, Tag::Add);
    r.clear();
    EXPECT_EQ(r.size(), 0U);
    EXPECT_EQ(r.dropped(), 0U);
    EXPECT_EQ(r.capacity(), 2U);
}

// ---------------------------------------------------------------------------
// Percentiles
// ---------------------------------------------------------------------------

TEST(Percentile, DesignWorkedExample) {
    // Design §2 and §5.2: ten operations, one of them slow.
    const std::vector<std::uint64_t> v{9, 10, 10, 10, 11, 11, 12, 12, 15, 480};
    const std::span<const std::uint64_t> s{v};
    EXPECT_EQ(nearest_rank(s, 0), 9U);      // min
    EXPECT_EQ(nearest_rank(s, kP50), 11U);  // ceil(0.5 × 10) − 1 = position 4
    EXPECT_EQ(nearest_rank(s, kP90), 15U);  // ceil(0.9 × 10) − 1 = position 8
    EXPECT_EQ(nearest_rank(s, kP99), 480U); // ceil(9.9) − 1 = position 9
    EXPECT_EQ(nearest_rank(s, kMax), 480U);
}

TEST(Percentile, OneSampleIsEveryPercentile) {
    const std::vector<std::uint64_t> v{7};
    for (const std::uint64_t q : {std::uint64_t{0}, kP50, kP999, kMax}) {
        EXPECT_EQ(nearest_rank(std::span<const std::uint64_t>{v}, q), 7U);
    }
}

TEST(Percentile, ExactRanksOnAThousandSamples) {
    std::vector<std::uint64_t> v(1'000);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = i + 1; // 1 .. 1000
    }
    const std::span<const std::uint64_t> s{v};
    EXPECT_EQ(nearest_rank(s, kP50), 500U);
    EXPECT_EQ(nearest_rank(s, kP90), 900U);
    EXPECT_EQ(nearest_rank(s, kP99), 990U);
    EXPECT_EQ(nearest_rank(s, kP999), 999U);
    EXPECT_EQ(nearest_rank(s, kMax), 1'000U);
}

TEST(Percentile, IntegerRankAvoidsFloatingPointRounding) {
    // In doubles, 0.07 × 100 = 7.000000000000001, and ceil() of that is 8: the wrong sample.
    // In millionths, 70'000 × 100 / 1'000'000 is exactly 7.
    std::vector<std::uint64_t> v(100);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = i + 1;
    }
    EXPECT_EQ(nearest_rank(std::span<const std::uint64_t>{v}, 70'000), 7U);
}

TEST(Summary, SubtractsTheOverheadButNeverBelowZero) {
    Recorder r{8};
    record_ticks(r, 3); // shorter than the overhead: becomes 0, not negative
    record_ticks(r, 10);
    record_ticks(r, 20);
    const Summary sum = summarize(r, 5, 1.0); // overhead 5 ticks, 1 tick per ns
    EXPECT_EQ(sum.count, 3U);
    EXPECT_DOUBLE_EQ(sum.min_ns, 0.0);
    EXPECT_DOUBLE_EQ(sum.p50_ns, 5.0);
    EXPECT_DOUBLE_EQ(sum.max_ns, 15.0);
    EXPECT_DOUBLE_EQ(sum.mean_ns, 20.0 / 3.0);
}

TEST(Summary, CanSelectOneTag) {
    Recorder r{8};
    record_ticks(r, 100, Tag::Add);
    record_ticks(r, 7, Tag::Reject);
    record_ticks(r, 300, Tag::Add);
    const Summary adds = summarize(r, 0, 2.0, Tag::Add); // 2 ticks per ns
    EXPECT_EQ(adds.count, 2U);
    EXPECT_DOUBLE_EQ(adds.min_ns, 50.0);
    EXPECT_DOUBLE_EQ(adds.max_ns, 150.0);
    EXPECT_EQ(summarize(r, 0, 2.0, Tag::Match).count, 0U); // no such samples
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

TEST(Histogram, ExactBucketsBelow1024) {
    EXPECT_EQ(bucket_of(0), 0U);
    EXPECT_EQ(bucket_of(500), 500U);
    EXPECT_EQ(bucket_of(1'023), 1'023U);
    EXPECT_EQ(bucket_low(1'023), 1'023U);
}

TEST(Histogram, EachDoublingSplitsInto64) {
    // 1,024 .. 2,047: 64 buckets of 16 ns.
    EXPECT_EQ(bucket_of(1'024), 1'024U);
    EXPECT_EQ(bucket_of(1'039), 1'024U);
    EXPECT_EQ(bucket_of(1'040), 1'025U);
    EXPECT_EQ(bucket_of(2'047), 1'087U);
    // 2,048 .. 4,095: 64 buckets of 32 ns.
    EXPECT_EQ(bucket_of(2'048), 1'088U);
    EXPECT_EQ(bucket_low(1'088), 2'048U);
    EXPECT_EQ(bucket_low(1'089), 2'080U);
}

TEST(Histogram, EveryValueIsInsideItsBucketAndBucketsAreNarrow) {
    for (std::uint64_t v = 0; v < 4'000'000'000; v = v * 5 / 4 + 1) {
        const std::size_t b = bucket_of(v);
        const std::uint64_t low = bucket_low(b);
        const std::uint64_t high = bucket_low(b + 1) - 1;
        EXPECT_LE(low, v);
        EXPECT_LE(v, high);
        if (b >= kExactBelow) {
            EXPECT_LE((high - low + 1) * 64, low); // at most 1/64 of its values wide
        }
    }
}

TEST(Histogram, CountsAddUpAndBucketsAscend) {
    const std::vector<std::uint64_t> sorted{1, 1, 2, 1'024, 1'030, 1'040, 50'000};
    const auto h = histogram(std::span<const std::uint64_t>{sorted}, 1.0);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        total += h[i].count;
        EXPECT_LE(h[i].low_ns, h[i].high_ns);
        if (i > 0) {
            EXPECT_LT(h[i - 1].high_ns, h[i].low_ns);
        }
    }
    EXPECT_EQ(total, sorted.size());
    ASSERT_EQ(h.size(), 5U); // {1, 1}, {2}, {1024, 1030}, {1040}, {50000}
    EXPECT_EQ(h[0].count, 2U);
    EXPECT_EQ(h[2].count, 2U);
}

// ---------------------------------------------------------------------------
// The planted slow operation: the harness's teeth (design §10)
// ---------------------------------------------------------------------------

TEST(TimeEach, SeesAPlantedSlowOperation) {
    // Every 500th operation stalls for 1.2 µs (0.2% of them); the rest do nothing. The tail must
    // show the stalls, and the typical operation must not.
    constexpr std::size_t kOps = 100'000;
    constexpr std::size_t kEvery = 500;
    const double rate = calibrate(1, 20'000'000).ticks_per_ns;
    const std::uint64_t overhead = measure_overhead(10'000).min;
    const auto stall_ticks = static_cast<std::uint64_t>(1'200.0 * rate);

    Recorder r{kOps};
    time_each(
        r, kOps,
        [&](std::size_t i) {
            if (i % kEvery == kEvery - 1) {
                const std::uint64_t until = read_stamp().ticks + stall_ticks;
                while (read_stamp().ticks < until) {
                }
            }
        },
        [](std::size_t) { return Tag::Other; });

    ASSERT_EQ(r.size() + r.dropped(), kOps);
    const Summary sum = summarize(r, overhead, rate);
    EXPECT_LT(sum.p50_ns, 1'000.0); // the typical operation: no stall
    EXPECT_LT(sum.p90_ns, 1'000.0);
    EXPECT_GE(sum.p999_ns, 1'000.0); // 0.2% stalled, so p99.9 lands on a stall
    EXPECT_GE(sum.max_ns, 1'000.0);

    // Every stall is seen: at least kOps / kEvery samples of 1 µs or more (fewer only if a
    // stalled sample was dropped because the thread moved core).
    const auto sorted = sorted_samples(r, overhead);
    const auto slow =
        std::ranges::count_if(sorted, [&](std::uint64_t t) { return to_ns(t, rate) >= 1'000.0; });
    EXPECT_GE(static_cast<std::uint64_t>(slow) + r.dropped(), kOps / kEvery);
}

// ---------------------------------------------------------------------------
// Environment (design §8)
// ---------------------------------------------------------------------------

TEST(Environment, ReadsCpuinfoFields) {
    constexpr std::string_view kCpuinfo = "processor\t: 0\n"
                                          "model name\t: Intel(R) Core(TM) i7-14650HX\n"
                                          "flags\t\t: fpu tsc\n"
                                          "\n"
                                          "processor\t: 1\n"
                                          "model name\t: Intel(R) Core(TM) i7-14650HX\n";
    EXPECT_EQ(cpuinfo_value(kCpuinfo, "model name"), "Intel(R) Core(TM) i7-14650HX");
    EXPECT_EQ(cpuinfo_value(kCpuinfo, "flags"), "fpu tsc");
    EXPECT_EQ(cpuinfo_value(kCpuinfo, "cache size"), ""); // absent
    EXPECT_EQ(count_processors(kCpuinfo), 2);
}

TEST(Environment, ReadsTheOsName) {
    EXPECT_EQ(os_pretty_name("NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 24.04.5 LTS\"\n"),
              "Ubuntu 24.04.5 LTS");
    EXPECT_EQ(os_pretty_name("PRETTY_NAME=Debian\n"), "Debian"); // quotes are optional
    EXPECT_EQ(os_pretty_name("NAME=x\n"), "");
}

TEST(Environment, RecognisesWsl) {
    EXPECT_TRUE(is_wsl("6.18.54.1-microsoft-standard-WSL2"));
    EXPECT_TRUE(is_wsl("4.4.0-19041-Microsoft")); // WSL1 spells it with a capital M
    EXPECT_FALSE(is_wsl("6.8.0-45-generic"));     // ordinary Ubuntu
}

TEST(Environment, ReadsTheTurboSetting) {
    EXPECT_EQ(turbo_from_no_turbo("1\n"), "off");
    EXPECT_EQ(turbo_from_no_turbo("0\n"), "on");
    EXPECT_EQ(turbo_from_no_turbo(""), "unknown"); // the file doesn't exist (e.g. WSL2)
}

TEST(Environment, TrimsWhitespace) {
    EXPECT_EQ(trim("  performance\n"), "performance");
    EXPECT_EQ(trim("\n"), "");
}

TEST(Environment, CollectsEveryField) {
    const Environment env = collect_environment(-1);
    for (const auto &[key, value] : env.lines()) {
        SCOPED_TRACE(key);
        EXPECT_FALSE(value.empty()); // "unknown" at worst, never empty
    }
}

TEST(Environment, PublishableOnlyFromBareMetalAndACleanCommit) {
    Environment env;
    env.virtual_machine = "no";
    env.commit = "9354158a1b2c";
    EXPECT_TRUE(env.publishable());
    EXPECT_EQ(env.unpublishable_reason(), "");

    env.commit = "9354158a1b2c-dirty"; // uncommitted changes: can't be traced to a commit
    EXPECT_FALSE(env.publishable());
    env.commit = "unknown"; // no git, or not a repository
    EXPECT_FALSE(env.publishable());

    env.commit = "9354158a1b2c";
    env.virtual_machine = "yes: WSL2";
    EXPECT_FALSE(env.publishable());
    EXPECT_NE(env.unpublishable_reason().find("virtual machine"), std::string::npos);
}

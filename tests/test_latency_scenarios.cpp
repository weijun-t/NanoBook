/*
 * test_latency_scenarios.cpp: tests for the latency harness's scenarios (phase 3).
 *
 * What it does: Checks classify() on hand-made event lists, that the shuffle is a seeded
 *   permutation, that each scenario records exactly the requested number of samples and times only
 *   the kind of operation it claims (every sample carries the expected tag), that the mixed stream
 *   contains every kind of operation, that every book sees exactly the same mixed inputs, and that
 *   throughput mode times the requested number of operations without recording samples.
 *
 * Why it exists: A scenario that quietly timed the wrong thing (a cancel that is really rejected, a
 *   "deep insert" that lands on an existing level) would give believable but wrong numbers. The
 *   tags make what was timed visible, so the tests can check it.
 *
 * Trade-offs: Uses small runs (a few thousand samples); the large-book scenarios still build their
 *   100,000-order book twice (warm-up and one batch), which takes a moment under the sanitizers.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "latency_recorder.hpp"
#include "latency_scenarios.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/reference/reference_book.hpp"

using namespace nanobook;
using namespace nanobook::latency;

namespace {

Tag tag_of(std::vector<Event> events) {
    return classify(events);
}

constexpr OrderId kA{7};
constexpr OrderId kB{3};

} // namespace

// ---------------------------------------------------------------------------
// classify()
// ---------------------------------------------------------------------------

TEST(Classify, EachKindOfInput) {
    EXPECT_EQ(tag_of({}), Tag::Other);
    EXPECT_EQ(tag_of({Event::make_new(kA)}), Tag::Add);
    EXPECT_EQ(tag_of({Event::make_new(kA), Event::make_trade(kA, kB, Price{10}, Qty{1})}),
              Tag::Match);
    EXPECT_EQ(tag_of({Event::make_canceled(kA, Qty{5}, CancelReason::Requested)}), Tag::Cancel);
    EXPECT_EQ(tag_of({Event::make_replaced(kA, Price{10}, Qty{50})}), Tag::Modify);
    EXPECT_EQ(tag_of({Event::make_rejected(kA, RejectReason::UnknownOrder)}), Tag::Reject);
}

TEST(Classify, AModifyThatCrossesIsAMatch) {
    EXPECT_EQ(tag_of({Event::make_replaced(kA, Price{10}, Qty{50}),
                      Event::make_trade(kA, kB, Price{10}, Qty{5})}),
              Tag::Match);
}

TEST(Classify, AnIocThatFindsNothingIsOther) {
    EXPECT_EQ(
        tag_of({Event::make_new(kA), Event::make_canceled(kA, Qty{5}, CancelReason::Unfilled)}),
        Tag::Other);
}

TEST(Classify, AnOrderThatRestsAfterASelfMatchCancelIsAnAdd) {
    // The canceled order is a resting one (kB), not the new order: the new order still rests.
    EXPECT_EQ(
        tag_of({Event::make_new(kA), Event::make_canceled(kB, Qty{5}, CancelReason::SelfMatch)}),
        Tag::Add);
}

// ---------------------------------------------------------------------------
// shuffle()
// ---------------------------------------------------------------------------

TEST(Shuffle, IsASeededPermutation) {
    std::vector<int> a(1'000);
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<int>(i);
    }
    std::vector<int> b = a;
    std::mt19937_64 rng_a{42};
    std::mt19937_64 rng_b{42};
    shuffle(a, rng_a);
    shuffle(b, rng_b);
    EXPECT_EQ(a, b); // same seed, same order
    EXPECT_FALSE(std::ranges::is_sorted(a));
    std::ranges::sort(a);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i], static_cast<int>(i)); // nothing lost, nothing repeated
    }
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

namespace {

struct Expected {
    Scenario scenario;
    Tag tag;
};

constexpr std::array<Expected, 8> kSingleKind = {{
    {Scenario::Add, Tag::Add},
    {Scenario::Cancel, Tag::Cancel},
    {Scenario::Modify, Tag::Modify},
    {Scenario::Match, Tag::Match},
    {Scenario::Sweep10, Tag::Match},
    {Scenario::LargeCancel, Tag::Cancel},
    {Scenario::LargeAdd, Tag::Add},
    {Scenario::DeepInsert, Tag::Add},
}};

} // namespace

TEST(Scenarios, EachTimesOnlyTheOperationItClaims) {
    constexpr std::size_t kOps = 3'000;
    for (const Expected &e : kSingleKind) {
        SCOPED_TRACE(scenario_name(e.scenario));
        Recorder r{kOps};
        run_scenario<FastBook>(e.scenario, r, Options{.ops = kOps});
        EXPECT_EQ(r.size() + r.dropped(), kOps);
        EXPECT_TRUE(std::ranges::all_of(r.tags(), [&](Tag t) { return t == e.tag; }));
    }
}

TEST(Scenarios, BatchesStopAtExactlyTheRequestedCount) {
    // 10,001 = one full batch of 10,000, then a batch of 1.
    constexpr std::size_t kOps = 10'001;
    Recorder r{kOps};
    run_scenario<FastBook>(Scenario::Add, r, Options{.ops = kOps});
    EXPECT_EQ(r.size() + r.dropped(), kOps);
    EXPECT_EQ(r.refused(), 0U);
}

TEST(Scenarios, MixedHasEveryKindOfOperation) {
    constexpr std::size_t kOps = 5'000;
    Recorder r{kOps};
    run_scenario<FastBook>(Scenario::Mixed, r, Options{.ops = kOps});
    for (const Tag t : {Tag::Add, Tag::Match, Tag::Cancel, Tag::Modify, Tag::Reject}) {
        SCOPED_TRACE(tag_name(t));
        EXPECT_GT(std::ranges::count(r.tags(), t), 0);
    }
}

TEST(Scenarios, EveryBookSeesTheSameMixedInputs) {
    // The same inputs into books that behave identically give the same tag, sample by sample.
    constexpr std::size_t kOps = 5'000;
    Recorder ref{kOps};
    Recorder fast{kOps};
    Recorder tick{kOps};
    run_scenario<ReferenceBook>(Scenario::Mixed, ref, Options{.ops = kOps});
    run_scenario<FastBook>(Scenario::Mixed, fast, Options{.ops = kOps});
    run_scenario<TickFastBook>(Scenario::Mixed, tick, Options{.ops = kOps});
    if (ref.dropped() + fast.dropped() + tick.dropped() > 0) {
        GTEST_SKIP() << "a sample was dropped (the thread moved core), so positions don't line up";
    }
    EXPECT_TRUE(std::ranges::equal(ref.tags(), fast.tags()));
    EXPECT_TRUE(std::ranges::equal(ref.tags(), tick.tags()));
}

TEST(Scenarios, ThroughputModeTimesWholeBatches) {
    constexpr std::size_t kOps = 15'000; // a batch of 10,000 and one of 5,000
    for (const Scenario s : {Scenario::Add, Scenario::Mixed}) {
        SCOPED_TRACE(scenario_name(s));
        Recorder r{1};
        const ScenarioResult result =
            run_scenario<FastBook>(s, r, Options{.ops = kOps, .throughput = true});
        EXPECT_EQ(result.throughput_ops, kOps);
        EXPECT_GT(result.throughput_ticks, 0U);
        EXPECT_EQ(r.size(), 0U); // nothing recorded one by one
    }
}

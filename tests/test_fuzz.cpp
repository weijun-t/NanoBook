/*
 * test_fuzz.cpp: proves the differential fuzzer works, before it is trusted with the fast book.
 *
 * What it does: The fast book against the reference book on several seeds, plus four kinds of
 *   checks on the fuzzer itself.
 *   1. No false alarms: reference vs. reference must always agree.
 *   2. It has teeth: two deliberately broken books must be caught. One emits wrong events (drops
 *      an IOC's closing Canceled). One reports a wrong level quantity while its events are right,
 *      which only the state comparison can catch.
 *   3. Replayable: the same seed gives the same input stream; a different seed doesn't.
 *   4. Steered: in a short run, the generator really produces every interesting case (trades,
 *      every cancel reason, the important reject reasons, a full book).
 *
 * Why it exists: A fuzzer that never fails proves nothing. It must be shown to pass when the books
 *   agree, and to fail when they don't.
 *
 * How it works: MutantBook wraps a ReferenceBook and breaks one thing, chosen by a template
 *   parameter. It still meets the OrderBook concept, so run() accepts it like any book. These
 *   tests run in every preset, including ASan and UBSan.
 */

#include <gtest/gtest.h>
#include "differential_fuzz.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <set>
#include <vector>

using namespace nanobook;
using namespace nanobook::fuzz;

namespace {

enum class Mutation : std::uint8_t { DropIocRemainder, WrongLevelQty };

// A reference book with one deliberate bug.
template <Mutation M> class MutantBook {
public:
    explicit MutantBook(BookConfig config) : inner_(config) {
    }

    void handle(const NewOrder &order, EventBuffer &out) {
        inner_.handle(order, out);
        if constexpr (M == Mutation::DropIocRemainder) {
            // Bug: an IOC's closing "Canceled{Unfilled}" receipt goes missing.
            const std::span<const Event> events = out.view();
            if (order.type == OrderType::IOC && events.size() > 1 &&
                events.back().cancel_reason == CancelReason::Unfilled) {
                const std::vector<Event> kept(events.begin(), events.end() - 1);
                out.clear();
                for (const Event &e : kept) {
                    out.push(e);
                }
            }
        }
    }
    void handle(const Cancel &cancel, EventBuffer &out) {
        inner_.handle(cancel, out);
    }
    void handle(const Modify &modify, EventBuffer &out) {
        inner_.handle(modify, out);
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        return inner_.best_bid();
    }
    [[nodiscard]] std::optional<Price> best_ask() const {
        return inner_.best_ask();
    }
    [[nodiscard]] Qty level_qty(Side side, Price price) const {
        const Qty q = inner_.level_qty(side, price);
        if constexpr (M == Mutation::WrongLevelQty) {
            return q > Qty{0} ? q + Qty{1} : q; // bug: every non-empty level reports one too many
        }
        return q;
    }
    [[nodiscard]] std::vector<OrderId> level_orders(Side side, Price price) const {
        return inner_.level_orders(side, price);
    }
    [[nodiscard]] std::optional<Qty> leaves(OrderId id) const {
        return inner_.leaves(id);
    }
    [[nodiscard]] std::size_t order_count() const {
        return inner_.order_count();
    }
    [[nodiscard]] std::vector<OrderId> resting_ids() const {
        return inner_.resting_ids();
    }
    [[nodiscard]] bool invariants_hold() const {
        return inner_.invariants_hold();
    }

private:
    ReferenceBook inner_;
};

static_assert(OrderBook<MutantBook<Mutation::DropIocRemainder>>);
static_assert(OrderBook<MutantBook<Mutation::WrongLevelQty>>);

} // namespace

// ---------------------------------------------------------------------------
// 1. No false alarms
// ---------------------------------------------------------------------------

TEST(Fuzz, ReferenceAgreesWithItself) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) { // includes both book sizes (see Generator)
        const auto mismatch =
            run<ReferenceBook, ReferenceBook>(Options{.seed = seed, .ops = 20'000});
        if (mismatch) {
            ADD_FAILURE() << report(*mismatch);
        }
    }
}

TEST(Fuzz, ReferenceAgreesWithItselfOnATinyBook) {
    const auto mismatch =
        run<ReferenceBook, ReferenceBook>(Options{.seed = 99, .ops = 20'000, .max_orders = 16});
    if (mismatch) {
        ADD_FAILURE() << report(*mismatch);
    }
}

// ---------------------------------------------------------------------------
// The real job: the fast book against the reference book
// ---------------------------------------------------------------------------

TEST(Fuzz, FastBookAgreesWithReference) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) { // includes both book sizes (see Generator)
        const auto mismatch = run<ReferenceBook, FastBook>(Options{.seed = seed, .ops = 20'000});
        if (mismatch) {
            ADD_FAILURE() << report(*mismatch);
        }
    }
}

TEST(Fuzz, TickFastBookAgreesWithReference) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const auto mismatch =
            run<ReferenceBook, TickFastBook>(Options{.seed = seed, .ops = 20'000});
        if (mismatch) {
            ADD_FAILURE() << report(*mismatch);
        }
    }
}

TEST(Fuzz, FastBookAgreesWithReferenceOnATinyBook) {
    const auto mismatch =
        run<ReferenceBook, FastBook>(Options{.seed = 99, .ops = 20'000, .max_orders = 16});
    if (mismatch) {
        ADD_FAILURE() << report(*mismatch);
    }
}

// ---------------------------------------------------------------------------
// 2. It has teeth
// ---------------------------------------------------------------------------

TEST(Fuzz, CatchesWrongEvents) {
    const auto mismatch = run<ReferenceBook, MutantBook<Mutation::DropIocRemainder>>(
        Options{.seed = 1, .ops = 20'000});
    if (!mismatch) {
        FAIL() << "the fuzzer missed a deliberate bug";
    }
    const Mismatch &m = *mismatch;
    EXPECT_EQ(m.what, "events differ");
    EXPECT_EQ(m.input.kind, Input::Kind::New);
    EXPECT_EQ(m.input.new_order.type, OrderType::IOC);
    EXPECT_EQ(m.events_a.size(), m.events_b.size() + 1); // B lost one event
}

TEST(Fuzz, CatchesWrongStateEvenWhenEventsMatch) {
    const auto mismatch =
        run<ReferenceBook, MutantBook<Mutation::WrongLevelQty>>(Options{.seed = 1, .ops = 20'000});
    if (!mismatch) {
        FAIL() << "the fuzzer missed a deliberate bug";
    }
    const Mismatch &m = *mismatch;
    EXPECT_NE(m.what.find("level_qty differs"), std::string::npos) << m.what;
    EXPECT_EQ(m.step % 64, 0U); // found at a state check (every 64 inputs by default)
}

TEST(Fuzz, ReportEndsWithTheReplayCommand) {
    const auto mismatch = run<ReferenceBook, MutantBook<Mutation::DropIocRemainder>>(
        Options{.seed = 7, .ops = 20'000});
    if (!mismatch) {
        FAIL() << "the fuzzer missed a deliberate bug";
    }
    const std::string text = report(*mismatch);
    EXPECT_NE(text.find("replay: nanobook_fuzz --seed 7 --ops " + std::to_string(mismatch->step)),
              std::string::npos)
        << text;
}

// ---------------------------------------------------------------------------
// 3. Replayable
// ---------------------------------------------------------------------------

namespace {

constexpr int kStreamLength = 2000;

// The first kStreamLength inputs of a seed, as text.
std::vector<std::string> stream(std::uint64_t seed) {
    Generator gen{seed};
    ReferenceBook book{gen.config()};
    EventBuffer out{gen.config().max_events_per_input()};
    std::vector<std::string> inputs;
    for (int i = 0; i < kStreamLength; ++i) {
        const Input in = gen.next(book);
        apply(book, in, out);
        gen.observe(out.view());
        inputs.push_back(describe(in));
    }
    return inputs;
}

} // namespace

TEST(Fuzz, SameSeedGivesTheSameStream) {
    EXPECT_EQ(stream(42), stream(42));
}

TEST(Fuzz, DifferentSeedsGiveDifferentStreams) {
    EXPECT_NE(stream(42), stream(43));
}

// ---------------------------------------------------------------------------
// 4. Steered: the interesting cases really happen
// ---------------------------------------------------------------------------

TEST(Fuzz, GeneratorCoversTheInterestingCases) {
    std::set<CancelReason> cancels;
    std::set<RejectReason> rejects;
    int trades = 0;
    int replaced = 0;

    for (const std::uint32_t max_orders : {16U, 2000U}) { // a tiny book makes BookFull common
        Generator gen{5, max_orders};
        ReferenceBook book{gen.config()};
        EventBuffer out{gen.config().max_events_per_input()};
        for (int i = 0; i < 20'000; ++i) {
            apply(book, gen.next(book), out);
            gen.observe(out.view());
            for (const Event &e : out.view()) {
                trades += e.type == EventType::Trade ? 1 : 0;
                replaced += e.type == EventType::Replaced ? 1 : 0;
                if (e.type == EventType::Canceled) {
                    cancels.insert(e.cancel_reason);
                }
                if (e.type == EventType::Rejected) {
                    rejects.insert(e.reject_reason);
                }
            }
        }
    }

    EXPECT_GT(trades, 1000);
    EXPECT_GT(replaced, 100);
    for (const CancelReason r : {CancelReason::Requested, CancelReason::SelfMatch,
                                 CancelReason::Unfilled, CancelReason::FokKill}) {
        EXPECT_TRUE(cancels.contains(r)) << "never saw Canceled " << name(r);
    }
    for (const RejectReason r :
         {RejectReason::InvalidQty, RejectReason::InvalidPrice, RejectReason::BookFull,
          RejectReason::NoLiquidity, RejectReason::UnknownOrder, RejectReason::TooLateToCancel,
          RejectReason::UseCancel, RejectReason::NoChange}) {
        EXPECT_TRUE(rejects.contains(r)) << "never saw Rejected " << name(r);
    }
}

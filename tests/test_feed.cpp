/*
 * test_feed.cpp: tests for the books' feed inputs (phase 4, ITCH design §6.1).
 *
 * What it does: Typed tests, run on all three books, one rule each: an add rests without
 *   matching even when it crosses; adds queue in arrival order; a reduce keeps the order's place,
 *   adds up over several reduces, and removes the order at 0; a remove takes it out of the middle
 *   of its queue; a replace gives a new id, keeps the side, goes to the back of the queue and
 *   tidies an emptied level. Then every failure (unknown id, duplicate id, bad qty or price,
 *   reduce too large, book full) in the contract's check order, each leaving the book unchanged.
 *   Last, a differential test: 100,000 random feed inputs (support/feed_fuzz.hpp) on all three
 *   books, which must give the same result after every input, and the same book (compared in full
 *   every 16th input and at the end, to stay well inside CI's 60-second limit under the
 *   sanitizers).
 *
 * Why it exists: Step 4 rebuilds Nasdaq's books with these four operations. A wrong one makes
 *   the copy drift from the real book without any crash, so each rule is tested on its own and
 *   then all mixed together.
 *
 * How it works: After every input, structure_holds() (every invariant except "not crossed") is
 *   checked. Errors are checked with a snapshot of the whole book before and after.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "differential_fuzz.hpp"
#include "feed_fuzz.hpp"
#include "test_helpers.hpp"

using namespace nanobook;
using namespace nanobook::test;

namespace {

constexpr BookConfig kConfig{
    .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 100, .max_qty = Qty{1'000'000}};

// Feed inputs in the test helpers' "qty @ price" order.
FeedAdd add_buy(std::uint64_t id, std::int64_t qty, std::int64_t price) {
    return FeedAdd{.id = OrderId{id}, .side = Side::Buy, .price = Price{price}, .qty = Qty{qty}};
}
FeedAdd add_sell(std::uint64_t id, std::int64_t qty, std::int64_t price) {
    return FeedAdd{.id = OrderId{id}, .side = Side::Sell, .price = Price{price}, .qty = Qty{qty}};
}
FeedReduce reduce(std::uint64_t id, std::int64_t qty) {
    return FeedReduce{.id = OrderId{id}, .qty = Qty{qty}};
}
FeedRemove remove_order(std::uint64_t id) {
    return FeedRemove{.id = OrderId{id}};
}
FeedReplace replace(std::uint64_t old_id, std::uint64_t new_id, std::int64_t qty,
                    std::int64_t price) {
    return FeedReplace{.old_id = OrderId{old_id},
                       .new_id = OrderId{new_id},
                       .price = Price{price},
                       .qty = Qty{qty}};
}

std::vector<OrderId> ids(std::initializer_list<std::uint64_t> values) {
    std::vector<OrderId> out;
    for (const std::uint64_t v : values) {
        out.push_back(OrderId{v});
    }
    return out;
}

// Everything a test can see of a book, to show that a failed input changed nothing.
struct Snapshot {
    std::vector<OrderId> resting{};
    std::optional<Price> bid{};
    std::optional<Price> ask{};
    std::vector<std::vector<OrderId>> queues{}; // each level from 90 to 110, both sides
    std::vector<Qty> totals{};

    bool operator==(const Snapshot &) const = default;
};

template <typename Book> Snapshot snapshot(const Book &book) {
    Snapshot s{.resting = book.resting_ids(), .bid = book.best_bid(), .ask = book.best_ask()};
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (std::int64_t p = 90; p <= 110; ++p) {
            s.queues.push_back(book.level_orders(side, Price{p}));
            s.totals.push_back(book.level_qty(side, Price{p}));
        }
    }
    return s;
}

template <typename Book> class FeedTest : public ::testing::Test {
protected:
    Book book{kConfig};

    // Applies one input, expects `result`, and checks the book's structure afterwards.
    template <typename Input> void apply(const Input &input, FeedResult result = FeedResult::Ok) {
        EXPECT_EQ(book.apply(input), result);
        EXPECT_TRUE(book.structure_holds());
    }
    // Expects `result`, and that the book is exactly as it was.
    template <typename Input> void rejected(const Input &input, FeedResult result) {
        const Snapshot before = snapshot(book);
        apply(input, result);
        EXPECT_EQ(snapshot(book), before);
    }
};

} // namespace

TYPED_TEST_SUITE(FeedTest, BookTypes, BookTypeNames);

// ---------------------------------------------------------------------------
// Each operation
// ---------------------------------------------------------------------------

TYPED_TEST(FeedTest, AddRestsWithoutMatchingEvenIfItCrosses) {
    this->apply(add_sell(1, 10, 100));
    this->apply(add_buy(2, 5, 105)); // would trade on our exchange; the feed already matched
    EXPECT_EQ(this->book.best_bid(), Price{105});
    EXPECT_EQ(this->book.best_ask(), Price{100});
    EXPECT_EQ(this->book.leaves(OrderId{1}), Qty{10});
    EXPECT_EQ(this->book.leaves(OrderId{2}), Qty{5});
    EXPECT_FALSE(this->book.invariants_hold()); // crossed, so not a valid exchange book...
    EXPECT_TRUE(this->book.structure_holds());  // ...but a valid copy of a crossed one
}

TYPED_TEST(FeedTest, AddsQueueInArrivalOrder) {
    this->apply(add_buy(7, 10, 100));
    this->apply(add_buy(3, 20, 100)); // ids needn't increase: the feed's order is what counts
    this->apply(add_buy(9, 30, 100));
    EXPECT_EQ(this->book.level_orders(Side::Buy, Price{100}), ids({7, 3, 9}));
    EXPECT_EQ(this->book.level_qty(Side::Buy, Price{100}), Qty{60});
}

TYPED_TEST(FeedTest, ReduceKeepsTheOrdersPlace) {
    this->apply(add_sell(1, 10, 100));
    this->apply(add_sell(2, 20, 100));
    this->apply(reduce(1, 4));
    EXPECT_EQ(this->book.leaves(OrderId{1}), Qty{6});
    EXPECT_EQ(this->book.level_orders(Side::Sell, Price{100}), ids({1, 2})); // still first
    EXPECT_EQ(this->book.level_qty(Side::Sell, Price{100}), Qty{26});
}

TYPED_TEST(FeedTest, ReducesAddUpAndRemoveTheOrderAtZero) {
    this->apply(add_buy(1, 10, 100));
    this->apply(reduce(1, 3));
    this->apply(reduce(1, 3));
    EXPECT_EQ(this->book.leaves(OrderId{1}), Qty{4});
    this->apply(reduce(1, 4));
    EXPECT_EQ(this->book.leaves(OrderId{1}), std::nullopt);
    EXPECT_EQ(this->book.order_count(), 0U);
    EXPECT_EQ(this->book.best_bid(), std::nullopt); // the empty level is gone too
}

TYPED_TEST(FeedTest, RemoveTakesAnOrderOutOfTheMiddleOfItsQueue) {
    this->apply(add_sell(1, 10, 100));
    this->apply(add_sell(2, 20, 100));
    this->apply(add_sell(3, 30, 100));
    this->apply(remove_order(2));
    EXPECT_EQ(this->book.level_orders(Side::Sell, Price{100}), ids({1, 3}));
    EXPECT_EQ(this->book.level_qty(Side::Sell, Price{100}), Qty{40});
}

TYPED_TEST(FeedTest, ReplaceGetsANewIdAndGoesToTheBackOfTheQueue) {
    this->apply(add_buy(1, 10, 100));
    this->apply(add_buy(2, 20, 100));
    this->apply(replace(1, 5, 7, 100)); // same price: it still loses its place
    EXPECT_EQ(this->book.level_orders(Side::Buy, Price{100}), ids({2, 5}));
    EXPECT_EQ(this->book.leaves(OrderId{1}), std::nullopt);
    EXPECT_EQ(this->book.leaves(OrderId{5}), Qty{7});
    EXPECT_EQ(this->book.level_qty(Side::Buy, Price{100}), Qty{27});
}

TYPED_TEST(FeedTest, ReplaceKeepsTheSideAndTidiesAnEmptiedLevel) {
    this->apply(add_sell(1, 10, 100));
    this->apply(replace(1, 2, 15, 103));
    EXPECT_EQ(this->book.level_orders(Side::Sell, Price{103}), ids({2})); // still a sell
    EXPECT_EQ(this->book.level_orders(Side::Sell, Price{100}), ids({}));  // level 100 is gone
    EXPECT_EQ(this->book.best_ask(), Price{103});
    EXPECT_EQ(this->book.best_bid(), std::nullopt);
}

TYPED_TEST(FeedTest, ReplaceMayKeepTheSameId) {
    this->apply(add_buy(1, 10, 100));
    this->apply(add_buy(2, 10, 100));
    this->apply(replace(1, 1, 8, 100));
    EXPECT_EQ(this->book.level_orders(Side::Buy, Price{100}), ids({2, 1}));
    EXPECT_EQ(this->book.leaves(OrderId{1}), Qty{8});
}

// ---------------------------------------------------------------------------
// Failures: each leaves the book exactly as it was
// ---------------------------------------------------------------------------

TYPED_TEST(FeedTest, AddFailuresInContractOrder) {
    this->apply(add_buy(1, 10, 100));
    this->rejected(add_buy(2, 0, 100), FeedResult::InvalidQty);
    this->rejected(add_buy(2, 1'000'001, 100), FeedResult::InvalidQty); // above max_qty
    this->rejected(add_buy(2, 10, 0), FeedResult::InvalidPrice);
    this->rejected(add_buy(2, 10, 1001), FeedResult::InvalidPrice);
    this->rejected(add_sell(1, 10, 101), FeedResult::DuplicateOrder); // id 1 is resting
    this->rejected(add_buy(2, 0, 0), FeedResult::InvalidQty);         // qty is checked first
    this->rejected(add_sell(1, 10, 0), FeedResult::InvalidPrice);     // price before duplicate
}

TYPED_TEST(FeedTest, AddToAFullBookFails) {
    TypeParam book{BookConfig{
        .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 2, .max_qty = Qty{100}}};
    EXPECT_EQ(book.apply(add_buy(1, 10, 100)), FeedResult::Ok);
    EXPECT_EQ(book.apply(add_buy(2, 10, 100)), FeedResult::Ok);
    EXPECT_EQ(book.apply(add_sell(3, 10, 90)), FeedResult::BookFull); // crossing doesn't help
    EXPECT_EQ(book.order_count(), 2U);
    // A replace frees its old place first, so it works even in a full book.
    EXPECT_EQ(book.apply(replace(1, 3, 5, 101)), FeedResult::Ok);
    EXPECT_TRUE(book.structure_holds());
}

TYPED_TEST(FeedTest, ReduceFailuresInContractOrder) {
    this->apply(add_buy(1, 10, 100));
    this->rejected(reduce(2, 5), FeedResult::UnknownOrder);
    this->rejected(reduce(2, 0), FeedResult::UnknownOrder); // the id is checked first
    this->rejected(reduce(1, 0), FeedResult::InvalidQty);
    this->rejected(reduce(1, -1), FeedResult::InvalidQty);
    this->rejected(reduce(1, 11), FeedResult::ReduceTooLarge); // only 10 left
}

TYPED_TEST(FeedTest, RemoveOfAnUnknownIdFails) {
    this->apply(add_buy(1, 10, 100));
    this->rejected(remove_order(2), FeedResult::UnknownOrder);
    this->apply(remove_order(1));
    this->rejected(remove_order(1), FeedResult::UnknownOrder); // gone already
}

TYPED_TEST(FeedTest, ReplaceFailuresInContractOrder) {
    this->apply(add_buy(1, 10, 100));
    this->apply(add_buy(2, 10, 100));
    this->rejected(replace(3, 4, 10, 100), FeedResult::UnknownOrder);
    this->rejected(replace(1, 4, 0, 100), FeedResult::InvalidQty);
    this->rejected(replace(1, 4, 10, 1001), FeedResult::InvalidPrice);
    this->rejected(replace(1, 2, 10, 100), FeedResult::DuplicateOrder); // id 2 is resting
    this->rejected(replace(3, 2, 0, 0), FeedResult::UnknownOrder);      // the old id comes first
}

// ---------------------------------------------------------------------------
// Everything mixed: the fast books must match the reference book after every input
// ---------------------------------------------------------------------------

TEST(FeedDifferential, AllBooksAgreeOnRandomFeedInputs) {
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 64, .max_qty = Qty{100}};
    const std::vector<support::FeedInput> inputs =
        support::random_feed({.seed = 42, .count = 100'000});

    ReferenceBook reference{config};
    FastBook fast{config};
    TickFastBook tick{config};
    WindowFastBook window{config};
    Window8FastBook window8{config};
    std::array<std::size_t, 7> seen{}; // how often each FeedResult happened

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const FeedResult expected = support::apply(reference, inputs[i]);
        ASSERT_EQ(support::apply(fast, inputs[i]), expected) << "FastBook, input " << i;
        ASSERT_EQ(support::apply(tick, inputs[i]), expected) << "TickFastBook, input " << i;
        ASSERT_EQ(support::apply(window, inputs[i]), expected) << "WindowFastBook, input " << i;
        ASSERT_EQ(support::apply(window8, inputs[i]), expected) << "Window8FastBook, input " << i;
        ++seen[static_cast<std::size_t>(expected)];

        ASSERT_TRUE(reference.structure_holds()) << "input " << i;
        ASSERT_TRUE(fast.structure_holds()) << "input " << i;
        ASSERT_TRUE(tick.structure_holds()) << "input " << i;
        ASSERT_TRUE(window.structure_holds()) << "input " << i;
        ASSERT_TRUE(window8.structure_holds()) << "input " << i;
        if (i % 16 != 15 && i + 1 != inputs.size()) {
            continue; // the full comparison is slow under the sanitizers: every 16th input
        }
        const std::optional<std::string> fast_diff = fuzz::compare_state(reference, fast);
        ASSERT_FALSE(fast_diff) << "FastBook, input " << i << ": " << fast_diff.value_or("");
        const std::optional<std::string> tick_diff = fuzz::compare_state(reference, tick);
        ASSERT_FALSE(tick_diff) << "TickFastBook, input " << i << ": " << tick_diff.value_or("");
        const std::optional<std::string> window_diff = fuzz::compare_state(reference, window);
        ASSERT_FALSE(window_diff) << "WindowFastBook, input " << i << ": "
                                  << window_diff.value_or("");
        const std::optional<std::string> window8_diff = fuzz::compare_state(reference, window8);
        ASSERT_FALSE(window8_diff)
            << "Window8FastBook, input " << i << ": " << window8_diff.value_or("");
    }
    // The stream really exercised every outcome, not only the easy ones.
    for (std::size_t r = 0; r < seen.size(); ++r) {
        EXPECT_GT(seen[r], 100U) << name(static_cast<FeedResult>(r));
    }
}

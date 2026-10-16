/*
 * test_book.cpp: behaviour tests for every order book.
 *
 * What it does: Covers every order type, every row of the design §10 table, partial fills, sweeps,
 *   FIFO order, cancel, modify (keeping or losing priority, the total-quantity rule), self-match
 *   prevention, and the capacity and quantity limits.
 *
 * Why it exists: The fast book must pass exactly the same tests as the reference book.
 *
 * How it works: Typed tests: each test is written once and runs on every book listed in BookTypes
 *   (test_helpers.hpp). The harness checks the book's invariants after every input. Test numbers
 *   (#1 to #32) refer to the design §10 table, whose notation is "owner/id qty@price".
 */

#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include <cstdint>
#include <limits>

using namespace nanobook;
using namespace nanobook::test;
using Events = std::vector<Event>;

namespace {

constexpr BookConfig kConfig{Price{1}, Price{1000}, 100, Qty{1'000'000}};

// Runs every test below once per type in BookTypes (test_helpers.hpp).
template <typename Book> class BookTest : public ::testing::Test {
protected:
    Harness<Book> h{kConfig};
};

// For tests that build their own book with a special config.
template <typename Book> class BookCapacityTest : public ::testing::Test {};

} // namespace

TYPED_TEST_SUITE(BookTest, BookTypes, BookTypeNames);
TYPED_TEST_SUITE(BookCapacityTest, BookTypes, BookTypeNames);

// ---------------------------------------------------------------------------
// Resting and matching limit orders
// ---------------------------------------------------------------------------

// #1
TYPED_TEST(BookTest, LimitOnEmptyBookRests) {
    EXPECT_EQ(this->h.send(buy(1, 1, 100, 10)), (Events{accepted(1)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{10});
    EXPECT_EQ(this->h.book.best_ask(), std::nullopt);
    EXPECT_EQ(this->h.book.level_qty(Side::Buy, Price{10}), Qty{100});
    EXPECT_EQ(this->h.book.order_count(), 1U);
}

// #2
TYPED_TEST(BookTest, FullFillRemovesBothOrders) {
    this->h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(this->h.send(buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 100, 10)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
    EXPECT_EQ(this->h.book.best_ask(), std::nullopt);
    EXPECT_EQ(this->h.book.best_bid(), std::nullopt);
}

// #3
TYPED_TEST(BookTest, PartialFillKeepsRestingOrderFirstInQueue) {
    this->h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(this->h.send(buy(1, 2, 40, 10)), (Events{accepted(2), trade(2, 1, 40, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{60});

    this->h.send(sell(3, 3, 50, 10)); // arrives later, so queues behind order 1
    EXPECT_EQ(this->h.book.level_orders(Side::Sell, Price{10}),
              (std::vector{OrderId{1}, OrderId{3}}));
}

// #4
TYPED_TEST(BookTest, SweepsSeveralLevels) {
    this->h.send(sell(2, 1, 50, 10));
    this->h.send(sell(2, 2, 50, 11));
    EXPECT_EQ(this->h.send(buy(1, 3, 80, 11)),
              (Events{accepted(3), trade(3, 1, 50, 10), trade(3, 2, 30, 11)}));
    EXPECT_EQ(this->h.book.level_qty(Side::Sell, Price{10}), Qty{0}); // level 10 is gone
    EXPECT_EQ(this->h.book.best_ask(), Price{11});
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{20});
}

// #5
TYPED_TEST(BookTest, TradesAtRestingPrice) {
    this->h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(this->h.send(buy(1, 2, 50, 12)), (Events{accepted(2), trade(2, 1, 50, 10)}));
}

TYPED_TEST(BookTest, OldestOrderAtAPriceMatchesFirst) {
    this->h.send(sell(2, 1, 30, 10));
    this->h.send(sell(3, 2, 30, 10));
    EXPECT_EQ(this->h.send(buy(1, 3, 40, 10)),
              (Events{accepted(3), trade(3, 1, 30, 10), trade(3, 2, 10, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{20});
}

TYPED_TEST(BookTest, BetterPriceMatchesBeforeOlderOrder) {
    this->h.send(sell(2, 1, 30, 11)); // older, but worse price
    this->h.send(sell(3, 2, 30, 10)); // newer, better price
    EXPECT_EQ(this->h.send(buy(1, 3, 30, 11)), (Events{accepted(3), trade(3, 2, 30, 10)}));
}

TYPED_TEST(BookTest, RemainderRestsAfterPartialMatch) {
    this->h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(this->h.send(buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 30, 10)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{10});
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{70});
}

TYPED_TEST(BookTest, SellAggressorMatchesHighestBidFirst) {
    this->h.send(buy(2, 1, 50, 9));
    this->h.send(buy(2, 2, 50, 10));
    EXPECT_EQ(this->h.send(sell(1, 3, 60, 9)),
              (Events{accepted(3), trade(3, 2, 50, 10), trade(3, 1, 10, 9)}));
}

TYPED_TEST(BookTest, NonCrossingOrdersBothRest) {
    this->h.send(buy(1, 1, 10, 9));
    EXPECT_EQ(this->h.send(sell(2, 2, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{9});
    EXPECT_EQ(this->h.book.best_ask(), Price{10});
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

// #6
TYPED_TEST(BookTest, CancelRemovesOrderAndEmptyLevel) {
    this->h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(this->h.send(cancel(1)), (Events{canceled(1, 100, CancelReason::Requested)}));
    EXPECT_EQ(this->h.book.best_ask(), std::nullopt);
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, CancelReportsOnlyRemainingQty) {
    this->h.send(sell(2, 1, 100, 10));
    this->h.send(buy(1, 2, 30, 10));
    EXPECT_EQ(this->h.send(cancel(1)), (Events{canceled(1, 70, CancelReason::Requested)}));
}

TYPED_TEST(BookTest, CancelMiddleOfQueueKeepsOthersInOrder) {
    this->h.send(sell(2, 1, 10, 10));
    this->h.send(sell(2, 2, 10, 10));
    this->h.send(sell(2, 3, 10, 10));
    this->h.send(cancel(2));
    EXPECT_EQ(this->h.book.level_orders(Side::Sell, Price{10}),
              (std::vector{OrderId{1}, OrderId{3}}));
}

// #7
TYPED_TEST(BookTest, CancelOfNeverSentIdIsUnknown) {
    EXPECT_EQ(this->h.send(cancel(9)), (Events{rejected(9, RejectReason::UnknownOrder)}));
}

// #8. A book only knows resting orders, so a finished order is UnknownOrder here. The exchange
// refines this to TooLateToCancel (test_exchange.cpp, E5).
TYPED_TEST(BookTest, CancelOfFilledOrderIsUnknownToTheBook) {
    this->h.send(sell(2, 1, 100, 10));
    this->h.send(buy(1, 2, 100, 10)); // fills order 1 completely
    EXPECT_EQ(this->h.send(cancel(1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(BookTest, CancelTwiceIsUnknownToTheBook) {
    this->h.send(sell(2, 1, 100, 10));
    this->h.send(cancel(1));
    EXPECT_EQ(this->h.send(cancel(1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

// #23 and #26 are gone: the exchange assigns ids, so a duplicate can't reach a book.

TYPED_TEST(BookTest, IdsMayHaveGaps) {
    // Each book sees exchange-wide ids: increasing, with gaps for other symbols' orders.
    EXPECT_EQ(this->h.send(buy(1, 1, 10, 10)), (Events{accepted(1)}));
    EXPECT_EQ(this->h.send(buy(1, 7, 10, 10)), (Events{accepted(7)}));
    EXPECT_EQ(this->h.send(sell(2, 42, 20, 10)),
              (Events{accepted(42), trade(42, 1, 10, 10), trade(42, 7, 10, 10)}));
}

// #24
TYPED_TEST(BookTest, ZeroOrNegativeQtyIsInvalid) {
    EXPECT_EQ(this->h.send(buy(1, 5, 0, 10)), (Events{rejected(5, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.send(buy(1, 6, -5, 10)), (Events{rejected(6, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

// #30, #31
TYPED_TEST(BookTest, PriceOutsideBandIsInvalid) {
    EXPECT_EQ(this->h.send(buy(1, 1, 10, 1001)), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->h.send(buy(1, 2, 10, 0)), (Events{rejected(2, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->h.send(buy(1, 3, 10, 1000)), (Events{accepted(3)})); // band edge is inside
}

TYPED_TEST(BookTest, ChecksRunInContractOrder) {
    // Bad qty AND bad price: only the first check (InvalidQty) is reported.
    EXPECT_EQ(this->h.send(buy(1, 6, 0, 0)), (Events{rejected(6, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// Capacity (design §4 "When exactly is the book full?")
// ---------------------------------------------------------------------------

// #28
TYPED_TEST(BookCapacityTest, FullBookRejectsOrderThatWouldRest) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    EXPECT_EQ(h.send(buy(1, 3, 10, 1)), (Events{rejected(3, RejectReason::BookFull)}));
    EXPECT_EQ(h.book.order_count(), 2U);
}

// #29
TYPED_TEST(BookCapacityTest, FullBookAcceptsOrderThatFillsCompletely) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    EXPECT_EQ(h.send(buy(1, 3, 10, 10)), (Events{accepted(3), trade(3, 1, 10, 10)}));
}

TYPED_TEST(BookCapacityTest, FullBookAcceptsCrossingOrderWithRemainder) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    // Crosses: uses up order 1, which frees its slot, so the remaining 5 can rest.
    EXPECT_EQ(h.send(buy(1, 3, 15, 10)), (Events{accepted(3), trade(3, 1, 10, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{3}), Qty{5});
    EXPECT_EQ(h.book.order_count(), 2U);
}

TYPED_TEST(BookCapacityTest, CancelFreesASlot) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
    h.send(buy(1, 1, 10, 10));
    EXPECT_EQ(h.send(buy(1, 2, 10, 10)), (Events{rejected(2, RejectReason::BookFull)}));
    h.send(cancel(1));
    EXPECT_EQ(h.send(buy(1, 3, 10, 10)), (Events{accepted(3)}));
}

// ---------------------------------------------------------------------------
// Market orders (design §5: any price, never rests, Rejected if nothing to trade against)
// ---------------------------------------------------------------------------

// #9
TYPED_TEST(BookTest, MarketOnEmptyBookIsRejected) {
    EXPECT_EQ(this->h.send(market_buy(1, 1, 100)),
              (Events{rejected(1, RejectReason::NoLiquidity)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, MarketWithOnlySameSideOrdersIsRejected) {
    this->h.send(buy(2, 1, 100, 10)); // a bid is no use to a market buy
    EXPECT_EQ(this->h.send(market_buy(1, 2, 100)),
              (Events{rejected(2, RejectReason::NoLiquidity)}));
}

// #10
TYPED_TEST(BookTest, MarketRemainderIsCanceledNotRested) {
    this->h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(this->h.send(market_buy(1, 2, 100)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 70, CancelReason::Unfilled)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
    EXPECT_EQ(this->h.book.best_bid(), std::nullopt);
}

TYPED_TEST(BookTest, MarketSweepsAnyPrice) {
    this->h.send(sell(2, 1, 50, 10));
    this->h.send(sell(2, 2, 50, 1000)); // top of the price band: still taken
    EXPECT_EQ(this->h.send(market_buy(1, 3, 80)),
              (Events{accepted(3), trade(3, 1, 50, 10), trade(3, 2, 30, 1000)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{20});
}

TYPED_TEST(BookTest, MarketSellTakesBidsHighestFirst) {
    this->h.send(buy(2, 1, 40, 1));
    this->h.send(buy(2, 2, 40, 10));
    EXPECT_EQ(this->h.send(market_sell(1, 3, 50)),
              (Events{accepted(3), trade(3, 2, 40, 10), trade(3, 1, 10, 1)}));
}

TYPED_TEST(BookTest, MarketPriceFieldIsIgnored) {
    this->h.send(sell(2, 1, 10, 10));
    NewOrder m = market_buy(1, 2, 10);
    m.price = Price{-999}; // outside the band, but a Market order has no price
    EXPECT_EQ(this->h.send(m), (Events{accepted(2), trade(2, 1, 10, 10)}));
}

TYPED_TEST(BookTest, MarketBadQtyIsReportedBeforeNoLiquidity) {
    EXPECT_EQ(this->h.send(market_buy(1, 1, 0)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// IOC (design §5: like a Limit order, but the remainder is canceled instead of resting)
// ---------------------------------------------------------------------------

// #11
TYPED_TEST(BookTest, IocOnEmptyBookIsCanceled) {
    EXPECT_EQ(this->h.send(ioc_buy(1, 1, 100, 10)),
              (Events{accepted(1), canceled(1, 100, CancelReason::Unfilled)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, IocPartialFillCancelsRemainder) {
    this->h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(this->h.send(ioc_buy(1, 2, 100, 10)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 70, CancelReason::Unfilled)}));
    EXPECT_EQ(this->h.book.best_bid(), std::nullopt); // nothing rested
}

TYPED_TEST(BookTest, IocRespectsItsLimitPrice) {
    this->h.send(sell(2, 1, 50, 10));
    this->h.send(sell(2, 2, 50, 12)); // above the IOC's limit of 11
    EXPECT_EQ(this->h.send(ioc_buy(1, 3, 100, 11)),
              (Events{accepted(3), trade(3, 1, 50, 10), canceled(3, 50, CancelReason::Unfilled)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{50}); // untouched
}

TYPED_TEST(BookTest, IocFullFillHasNoCancel) {
    this->h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(this->h.send(ioc_buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 100, 10)}));
}

TYPED_TEST(BookTest, IocSellTakesBids) {
    this->h.send(buy(2, 1, 30, 10));
    EXPECT_EQ(this->h.send(ioc_sell(1, 2, 50, 10)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 20, CancelReason::Unfilled)}));
}

TYPED_TEST(BookTest, IocPriceOutsideBandIsInvalid) {
    EXPECT_EQ(this->h.send(ioc_buy(1, 1, 10, 0)),
              (Events{rejected(1, RejectReason::InvalidPrice)}));
}

// ---------------------------------------------------------------------------
// FOK (design §5: look first, then fill completely or kill; the book is never half-changed)
// ---------------------------------------------------------------------------

// #12
TYPED_TEST(BookTest, FokWithoutEnoughQtyIsKilledAndBookUnchanged) {
    this->h.send(sell(2, 1, 60, 10));
    EXPECT_EQ(this->h.send(fok_buy(1, 2, 100, 10)),
              (Events{accepted(2), canceled(2, 100, CancelReason::FokKill)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{60}); // nothing traded
}

// #13
TYPED_TEST(BookTest, FokWithExactlyEnoughFillsCompletely) {
    this->h.send(sell(2, 1, 60, 10));
    this->h.send(sell(2, 2, 40, 10));
    EXPECT_EQ(this->h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), trade(3, 1, 60, 10), trade(3, 2, 40, 10)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, FokOnlyCountsAcceptablePrices) {
    this->h.send(sell(2, 1, 60, 10));
    this->h.send(sell(2, 2, 40, 11)); // enough in total, but 11 is above the FOK's limit
    EXPECT_EQ(this->h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), canceled(3, 100, CancelReason::FokKill)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{60});
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{40});
}

TYPED_TEST(BookTest, FokFillsAcrossLevelsWithinItsLimit) {
    this->h.send(sell(2, 1, 60, 10));
    this->h.send(sell(2, 2, 40, 11));
    EXPECT_EQ(this->h.send(fok_buy(1, 3, 100, 11)),
              (Events{accepted(3), trade(3, 1, 60, 10), trade(3, 2, 40, 11)}));
}

TYPED_TEST(BookTest, FokSellAndEmptyBook) {
    EXPECT_EQ(this->h.send(fok_sell(1, 1, 10, 10)),
              (Events{accepted(1), canceled(1, 10, CancelReason::FokKill)}));
    this->h.send(buy(2, 2, 15, 10));
    EXPECT_EQ(this->h.send(fok_sell(1, 3, 10, 10)), (Events{accepted(3), trade(3, 2, 10, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{5});
}

// ---------------------------------------------------------------------------
// Only Limit orders rest, so only Limit orders can be BookFull
// ---------------------------------------------------------------------------

TYPED_TEST(BookCapacityTest, NonRestingTypesAreNeverBookFull) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
    h.send(buy(2, 1, 10, 5)); // book is now full
    EXPECT_EQ(h.send(ioc_buy(1, 2, 10, 5)),
              (Events{accepted(2), canceled(2, 10, CancelReason::Unfilled)}));
    EXPECT_EQ(h.send(fok_buy(1, 3, 10, 5)),
              (Events{accepted(3), canceled(3, 10, CancelReason::FokKill)}));
    EXPECT_EQ(h.send(market_buy(1, 4, 10)), (Events{rejected(4, RejectReason::NoLiquidity)}));
    EXPECT_EQ(h.send(market_sell(1, 5, 10)), (Events{accepted(5), trade(5, 1, 10, 5)}));
}

// ---------------------------------------------------------------------------
// Self-match prevention (design §6: cancel the owner's own resting order, keep matching)
// ---------------------------------------------------------------------------

// #14
TYPED_TEST(BookTest, SelfMatchCancelsOwnRestingOrderThenTrades) {
    this->h.send(sell(1, 1, 50, 10)); // owner 1's own order, first in the queue
    this->h.send(sell(2, 2, 50, 10));
    EXPECT_EQ(this->h.send(buy(1, 3, 50, 10)),
              (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 50, 10)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

// #15
TYPED_TEST(BookTest, MarketAgainstOnlyOwnOrdersIsRejectedAndOwnOrdersKept) {
    this->h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(this->h.send(market_buy(1, 2, 50)), (Events{rejected(2, RejectReason::NoLiquidity)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{50}); // not canceled
}

// #16
TYPED_TEST(BookTest, FokDoesNotCountOwnOrders) {
    this->h.send(sell(1, 1, 50, 10));
    this->h.send(sell(2, 2, 50, 10)); // 100 in total, but only 50 from other owners
    EXPECT_EQ(this->h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), canceled(3, 100, CancelReason::FokKill)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}),
              Qty{50}); // killed FOK touches nothing, own order too
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{50});
}

TYPED_TEST(BookTest, FokCancelsOwnOrdersItPassesWhenEnoughOthers) {
    this->h.send(sell(1, 1, 50, 10));  // own, better price
    this->h.send(sell(2, 2, 100, 11)); // enough from another owner within the limit
    EXPECT_EQ(
        this->h.send(fok_buy(1, 3, 100, 11)),
        (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 100, 11)}));
}

TYPED_TEST(BookTest, MarketCancelsOwnOrdersOnTheWay) {
    this->h.send(sell(1, 1, 50, 10));
    this->h.send(sell(2, 2, 50, 11));
    EXPECT_EQ(this->h.send(market_buy(1, 3, 50)),
              (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 50, 11)}));
}

TYPED_TEST(BookTest, IocAgainstOnlyOwnOrders) {
    this->h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(this->h.send(ioc_buy(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch),
                      canceled(2, 50, CancelReason::Unfilled)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, LimitAgainstOnlyOwnOrdersRestsAfterCancelingThem) {
    this->h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(this->h.send(buy(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{10}); // order 2 rests; the book is not crossed
    EXPECT_EQ(this->h.book.best_ask(), std::nullopt);
}

TYPED_TEST(BookTest, SelfMatchReportsOnlyRemainingQty) {
    this->h.send(sell(1, 1, 100, 10));
    this->h.send(buy(2, 2, 40, 10)); // another owner takes 40 of order 1
    EXPECT_EQ(this->h.send(buy(1, 3, 10, 10)),
              (Events{accepted(3), canceled(1, 60, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{3}), Qty{10});
}

TYPED_TEST(BookTest, OwnOrdersBeyondTheFillAreUntouched) {
    this->h.send(sell(2, 1, 50, 10));
    this->h.send(sell(1, 2, 50, 10)); // own order, but behind order 1
    EXPECT_EQ(this->h.send(buy(1, 3, 50, 10)), (Events{accepted(3), trade(3, 1, 50, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{2}), Qty{50}); // never reached, so not canceled
}

TYPED_TEST(BookTest, SelfMatchOnSellSide) {
    this->h.send(buy(1, 1, 50, 10));
    EXPECT_EQ(this->h.send(sell(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->h.book.best_ask(), Price{10});
}

TYPED_TEST(BookCapacityTest, CrossingOnlyOwnOrderOnFullBookFreesTheSlot) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
    h.send(sell(1, 1, 10, 10)); // book is full
    EXPECT_EQ(h.send(buy(1, 2, 10, 10)),
              (Events{accepted(2), canceled(1, 10, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.order_count(), 1U); // order 2 took the freed slot
}

// ---------------------------------------------------------------------------
// Modify (design §7). Helper order: modify(id, new TOTAL qty, new price).
// ---------------------------------------------------------------------------

// #17
TYPED_TEST(BookTest, ModifyDownKeepsPlaceInQueue) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, 60, 10)), (Events{replaced(1, 60, 10)}));
    EXPECT_EQ(this->h.book.level_orders(Side::Buy, Price{10}),
              (std::vector{OrderId{1}, OrderId{2}}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{60});
}

// #18
TYPED_TEST(BookTest, ModifyUpLosesPlaceInQueue) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, 150, 10)), (Events{replaced(1, 150, 10)}));
    EXPECT_EQ(this->h.book.level_orders(Side::Buy, Price{10}),
              (std::vector{OrderId{2}, OrderId{1}}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{150});
}

// #19: the race from design §7. 30 traded before the modify arrived, so total 50 leaves 20.
TYPED_TEST(BookTest, ModifyTotalCountsWhatAlreadyTraded) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 30, 10)); // 30 of order 1 trade
    EXPECT_EQ(this->h.send(modify(1, 50, 10)), (Events{replaced(1, 50, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{20}); // not 50: no overfill
}

TYPED_TEST(BookTest, ModifyUpCountsWhatAlreadyTraded) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 30, 10)); // 30 of order 1 trade
    EXPECT_EQ(this->h.send(modify(1, 150, 10)), (Events{replaced(1, 150, 10)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{120}); // 150 total - 30 traded, not 150
}

// #20
TYPED_TEST(BookTest, ModifyTotalAtOrBelowFilledIsTooLate) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 30, 10));
    EXPECT_EQ(this->h.send(modify(1, 30, 10)),
              (Events{rejected(1, RejectReason::TooLateToCancel)}));
    EXPECT_EQ(this->h.send(modify(1, 20, 10)),
              (Events{rejected(1, RejectReason::TooLateToCancel)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{70}); // unchanged
}

// #21
TYPED_TEST(BookTest, ModifyToZeroMustUseCancel) {
    this->h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, 0, 10)), (Events{rejected(1, RejectReason::UseCancel)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{100});
}

// #22
TYPED_TEST(BookTest, ModifyToCrossingPriceTradesImmediately) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 100, 12));
    EXPECT_EQ(this->h.send(modify(1, 100, 12)),
              (Events{replaced(1, 100, 12), trade(1, 2, 100, 12)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

// #25
TYPED_TEST(BookTest, ModifyThatChangesNothingIsRejected) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, 100, 10)), (Events{rejected(1, RejectReason::NoChange)}));
    EXPECT_EQ(this->h.book.level_orders(Side::Buy, Price{10}),
              (std::vector{OrderId{1}, OrderId{2}}));
}

TYPED_TEST(BookTest, ModifyOfNeverSentIdIsUnknown) {
    EXPECT_EQ(this->h.send(modify(9, 10, 10)), (Events{rejected(9, RejectReason::UnknownOrder)}));
}

// A book only knows resting orders; the exchange refines these to TooLateToCancel (E9).
TYPED_TEST(BookTest, ModifyOfFinishedOrderIsUnknownToTheBook) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 100, 10)); // order 1 fully filled
    this->h.send(buy(1, 3, 100, 10));
    this->h.send(cancel(3)); // order 3 canceled
    EXPECT_EQ(this->h.send(modify(1, 50, 10)), (Events{rejected(1, RejectReason::UnknownOrder)}));
    EXPECT_EQ(this->h.send(modify(3, 50, 10)), (Events{rejected(3, RejectReason::UnknownOrder)}));
}

TYPED_TEST(BookTest, ModifyOfIocIsUnknownToTheBook) {
    this->h.send(ioc_buy(1, 1, 100, 10)); // never rests, finished straight away
    EXPECT_EQ(this->h.send(modify(1, 50, 10)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(BookTest, ModifyInvalidQtyAndPrice) {
    this->h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, -5, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.send(modify(1, 50, 0)), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->h.send(modify(1, 50, 1001)), (Events{rejected(1, RejectReason::InvalidPrice)}));
}

TYPED_TEST(BookTest, ModifyChecksRunInContractOrder) {
    this->h.send(buy(1, 1, 100, 10));
    // Total 0 AND bad price: UseCancel (step 3) comes before InvalidPrice (step 4).
    EXPECT_EQ(this->h.send(modify(1, 0, 0)), (Events{rejected(1, RejectReason::UseCancel)}));
    // Negative total AND bad price: InvalidQty comes before InvalidPrice.
    EXPECT_EQ(this->h.send(modify(1, -1, 0)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

TYPED_TEST(BookTest, ModifyPriceLosesPlaceEvenWhenShrinking) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(buy(2, 2, 100, 9));
    EXPECT_EQ(this->h.send(modify(1, 50, 9)), (Events{replaced(1, 50, 9)}));
    EXPECT_EQ(this->h.book.level_orders(Side::Buy, Price{9}),
              (std::vector{OrderId{2}, OrderId{1}}));
    EXPECT_EQ(this->h.book.best_bid(), Price{9}); // level 10 emptied and is gone
}

TYPED_TEST(BookTest, ModifyCrossingRemainderRestsAtNewPrice) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 40, 12));
    EXPECT_EQ(this->h.send(modify(1, 100, 12)),
              (Events{replaced(1, 100, 12), trade(1, 2, 40, 12)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{12});
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{60});
}

TYPED_TEST(BookTest, ModifyCrossingOnlyTradesWhatIsLeft) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(2, 2, 30, 10));  // 30 of order 1 trade
    this->h.send(sell(3, 3, 100, 12)); // plenty available at 12
    // Total 100 with 30 already traded: only 70 more may trade.
    EXPECT_EQ(this->h.send(modify(1, 100, 12)),
              (Events{replaced(1, 100, 12), trade(1, 3, 70, 12)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), std::nullopt); // complete
    EXPECT_EQ(this->h.book.leaves(OrderId{3}), Qty{30});
}

TYPED_TEST(BookTest, ModifyCrossingOwnOrderTriggersSelfMatch) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(sell(1, 2, 100, 12)); // same owner
    EXPECT_EQ(this->h.send(modify(1, 100, 12)),
              (Events{replaced(1, 100, 12), canceled(2, 100, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->h.book.best_bid(), Price{12});
    EXPECT_EQ(this->h.book.best_ask(), std::nullopt);
}

TYPED_TEST(BookTest, ModifySellSideKeepsAndLosesPlace) {
    this->h.send(sell(1, 1, 100, 10));
    this->h.send(sell(2, 2, 100, 10));
    this->h.send(modify(1, 40, 10)); // down: keeps place
    EXPECT_EQ(this->h.book.level_orders(Side::Sell, Price{10}),
              (std::vector{OrderId{1}, OrderId{2}}));
    this->h.send(modify(1, 80, 10)); // up: loses place
    EXPECT_EQ(this->h.book.level_orders(Side::Sell, Price{10}),
              (std::vector{OrderId{2}, OrderId{1}}));
}

TYPED_TEST(BookTest, ModifiedOrderCanBeModifiedAndCanceledAgain) {
    this->h.send(buy(1, 1, 100, 10));
    this->h.send(modify(1, 120, 11));
    this->h.send(modify(1, 90, 11));
    EXPECT_EQ(this->h.send(cancel(1)), (Events{canceled(1, 90, CancelReason::Requested)}));
    EXPECT_EQ(this->h.book.order_count(), 0U);
}

TYPED_TEST(BookCapacityTest, ModifyIsNeverBookFull) {
    Harness<TypeParam> h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(buy(1, 1, 10, 10));
    h.send(buy(1, 2, 10, 9));                                          // book is full
    EXPECT_EQ(h.send(modify(1, 20, 8)), (Events{replaced(1, 20, 8)})); // frees its own slot first
    EXPECT_EQ(h.book.order_count(), 2U);
}

// ---------------------------------------------------------------------------
// Quantity limit (design §12: max_qty keeps every sum inside 64 bits)
// ---------------------------------------------------------------------------

// #32
TYPED_TEST(BookTest, QtyAboveMaxIsInvalid) {
    EXPECT_EQ(this->h.send(buy(1, 1, 1'000'001, 10)),
              (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.send(buy(1, 2, 1'000'000, 10)),
              (Events{accepted(2)})); // exactly max: allowed
}

TYPED_TEST(BookTest, ModifyTotalAboveMaxIsInvalid) {
    this->h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(this->h.send(modify(1, 1'000'001, 10)),
              (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.book.leaves(OrderId{1}), Qty{100});
}

// The overflow found in review: two huge orders summed past 2^63 inside the FOK look-first count.
// A config that allows that is now invalid, and with a valid config such orders are rejected.
TYPED_TEST(BookCapacityTest, HugeQuantitiesCannotOverflow) {
    constexpr std::int64_t big = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    EXPECT_FALSE((BookConfig{Price{1}, Price{1000}, 10, Qty{big}}.is_valid()));

    Harness<TypeParam> h{kConfig};
    EXPECT_EQ(h.send(sell(2, 1, big, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// Found in review: correct already, now locked in by tests
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest, SelfMatchInMiddleOfQueue) {
    this->h.send(sell(2, 1, 10, 10));
    this->h.send(sell(1, 2, 10, 10)); // own order, between two others
    this->h.send(sell(3, 3, 10, 10));
    EXPECT_EQ(this->h.send(buy(1, 4, 20, 10)),
              (Events{accepted(4), trade(4, 1, 10, 10), canceled(2, 10, CancelReason::SelfMatch),
                      trade(4, 3, 10, 10)}));
}

// One input can produce max_orders + 2 events (design §12). Build that exact worst case, so the
// EventBuffer's assert would fire if the formula were ever too small.
TYPED_TEST(BookCapacityTest, WorstCaseEventCountFitsBuffer) {
    constexpr BookConfig cfg{Price{1}, Price{1000}, 50, Qty{1'000'000}};
    Harness<TypeParam> h{cfg};
    for (std::int64_t i = 1; i <= 50; ++i) {
        h.send(sell(2, static_cast<std::uint64_t>(i), 1, i)); // 50 orders on 50 levels: full
    }
    const Events events = h.send(ioc_buy(1, 51, 1000, 1000)); // New + 50 trades + Canceled
    EXPECT_EQ(events.size(), cfg.max_events_per_input());
    EXPECT_EQ(events.back(), canceled(51, 950, CancelReason::Unfilled));
}

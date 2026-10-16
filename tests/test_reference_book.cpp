#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include <cstdint>
#include <limits>

// Test numbers refer to the design §10 table. Notation there: "owner/id qty@price".

using namespace nanobook;
using namespace nanobook::test;
using Events = std::vector<Event>;

namespace {

constexpr BookConfig kConfig{Price{1}, Price{1000}, 100, Qty{1'000'000}};

class ReferenceBookTest : public ::testing::Test {
protected:
    Harness h{kConfig};
};

} // namespace

// ---------------------------------------------------------------------------
// Resting and matching limit orders
// ---------------------------------------------------------------------------

// #1
TEST_F(ReferenceBookTest, LimitOnEmptyBookRests) {
    EXPECT_EQ(h.send(buy(1, 1, 100, 10)), (Events{accepted(1)}));
    EXPECT_EQ(h.book.best_bid(), Price{10});
    EXPECT_EQ(h.book.best_ask(), std::nullopt);
    EXPECT_EQ(h.book.level_qty(Side::Buy, Price{10}), Qty{100});
    EXPECT_EQ(h.book.order_count(), 1U);
}

// #2
TEST_F(ReferenceBookTest, FullFillRemovesBothOrders) {
    h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(h.send(buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 100, 10)}));
    EXPECT_EQ(h.book.order_count(), 0U);
    EXPECT_EQ(h.book.best_ask(), std::nullopt);
    EXPECT_EQ(h.book.best_bid(), std::nullopt);
}

// #3
TEST_F(ReferenceBookTest, PartialFillKeepsRestingOrderFirstInQueue) {
    h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(h.send(buy(1, 2, 40, 10)), (Events{accepted(2), trade(2, 1, 40, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{60});

    h.send(sell(3, 3, 50, 10)); // arrives later, so queues behind order 1
    EXPECT_EQ(h.book.level_orders(Side::Sell, Price{10}), (std::vector{OrderId{1}, OrderId{3}}));
}

// #4
TEST_F(ReferenceBookTest, SweepsSeveralLevels) {
    h.send(sell(2, 1, 50, 10));
    h.send(sell(2, 2, 50, 11));
    EXPECT_EQ(h.send(buy(1, 3, 80, 11)),
              (Events{accepted(3), trade(3, 1, 50, 10), trade(3, 2, 30, 11)}));
    EXPECT_EQ(h.book.level_qty(Side::Sell, Price{10}), Qty{0}); // level 10 is gone
    EXPECT_EQ(h.book.best_ask(), Price{11});
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{20});
}

// #5
TEST_F(ReferenceBookTest, TradesAtRestingPrice) {
    h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(h.send(buy(1, 2, 50, 12)), (Events{accepted(2), trade(2, 1, 50, 10)}));
}

TEST_F(ReferenceBookTest, OldestOrderAtAPriceMatchesFirst) {
    h.send(sell(2, 1, 30, 10));
    h.send(sell(3, 2, 30, 10));
    EXPECT_EQ(h.send(buy(1, 3, 40, 10)),
              (Events{accepted(3), trade(3, 1, 30, 10), trade(3, 2, 10, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{20});
}

TEST_F(ReferenceBookTest, BetterPriceMatchesBeforeOlderOrder) {
    h.send(sell(2, 1, 30, 11)); // older, but worse price
    h.send(sell(3, 2, 30, 10)); // newer, better price
    EXPECT_EQ(h.send(buy(1, 3, 30, 11)), (Events{accepted(3), trade(3, 2, 30, 10)}));
}

TEST_F(ReferenceBookTest, RemainderRestsAfterPartialMatch) {
    h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(h.send(buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 30, 10)}));
    EXPECT_EQ(h.book.best_bid(), Price{10});
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{70});
}

TEST_F(ReferenceBookTest, SellAggressorMatchesHighestBidFirst) {
    h.send(buy(2, 1, 50, 9));
    h.send(buy(2, 2, 50, 10));
    EXPECT_EQ(h.send(sell(1, 3, 60, 9)),
              (Events{accepted(3), trade(3, 2, 50, 10), trade(3, 1, 10, 9)}));
}

TEST_F(ReferenceBookTest, NonCrossingOrdersBothRest) {
    h.send(buy(1, 1, 10, 9));
    EXPECT_EQ(h.send(sell(2, 2, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(h.book.best_bid(), Price{9});
    EXPECT_EQ(h.book.best_ask(), Price{10});
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

// #6
TEST_F(ReferenceBookTest, CancelRemovesOrderAndEmptyLevel) {
    h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(h.send(cancel(1)), (Events{canceled(1, 100, CancelReason::Requested)}));
    EXPECT_EQ(h.book.best_ask(), std::nullopt);
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, CancelReportsOnlyRemainingQty) {
    h.send(sell(2, 1, 100, 10));
    h.send(buy(1, 2, 30, 10));
    EXPECT_EQ(h.send(cancel(1)), (Events{canceled(1, 70, CancelReason::Requested)}));
}

TEST_F(ReferenceBookTest, CancelMiddleOfQueueKeepsOthersInOrder) {
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 10));
    h.send(sell(2, 3, 10, 10));
    h.send(cancel(2));
    EXPECT_EQ(h.book.level_orders(Side::Sell, Price{10}), (std::vector{OrderId{1}, OrderId{3}}));
}

// #7
TEST_F(ReferenceBookTest, CancelOfNeverSentIdIsUnknown) {
    EXPECT_EQ(h.send(cancel(9)), (Events{rejected(9, RejectReason::UnknownOrder)}));
}

// #8. A book only knows resting orders, so a finished order is UnknownOrder here. The exchange
// refines this to TooLateToCancel (test_exchange.cpp, E5).
TEST_F(ReferenceBookTest, CancelOfFilledOrderIsUnknownToTheBook) {
    h.send(sell(2, 1, 100, 10));
    h.send(buy(1, 2, 100, 10)); // fills order 1 completely
    EXPECT_EQ(h.send(cancel(1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TEST_F(ReferenceBookTest, CancelTwiceIsUnknownToTheBook) {
    h.send(sell(2, 1, 100, 10));
    h.send(cancel(1));
    EXPECT_EQ(h.send(cancel(1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

// #23 and #26 are gone: the exchange assigns ids, so a duplicate can't reach a book.

TEST_F(ReferenceBookTest, IdsMayHaveGaps) {
    // Each book sees exchange-wide ids: increasing, with gaps for other symbols' orders.
    EXPECT_EQ(h.send(buy(1, 1, 10, 10)), (Events{accepted(1)}));
    EXPECT_EQ(h.send(buy(1, 7, 10, 10)), (Events{accepted(7)}));
    EXPECT_EQ(h.send(sell(2, 42, 20, 10)),
              (Events{accepted(42), trade(42, 1, 10, 10), trade(42, 7, 10, 10)}));
}

// #24
TEST_F(ReferenceBookTest, ZeroOrNegativeQtyIsInvalid) {
    EXPECT_EQ(h.send(buy(1, 5, 0, 10)), (Events{rejected(5, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.send(buy(1, 6, -5, 10)), (Events{rejected(6, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

// #30, #31
TEST_F(ReferenceBookTest, PriceOutsideBandIsInvalid) {
    EXPECT_EQ(h.send(buy(1, 1, 10, 1001)), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(h.send(buy(1, 2, 10, 0)), (Events{rejected(2, RejectReason::InvalidPrice)}));
    EXPECT_EQ(h.send(buy(1, 3, 10, 1000)), (Events{accepted(3)})); // band edge is inside
}

TEST_F(ReferenceBookTest, ChecksRunInContractOrder) {
    // Bad qty AND bad price: only the first check (InvalidQty) is reported.
    EXPECT_EQ(h.send(buy(1, 6, 0, 0)), (Events{rejected(6, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// Capacity (design §4 "When exactly is the book full?")
// ---------------------------------------------------------------------------

// #28
TEST(ReferenceBookCapacity, FullBookRejectsOrderThatWouldRest) {
    Harness h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    EXPECT_EQ(h.send(buy(1, 3, 10, 1)), (Events{rejected(3, RejectReason::BookFull)}));
    EXPECT_EQ(h.book.order_count(), 2U);
}

// #29
TEST(ReferenceBookCapacity, FullBookAcceptsOrderThatFillsCompletely) {
    Harness h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    EXPECT_EQ(h.send(buy(1, 3, 10, 10)), (Events{accepted(3), trade(3, 1, 10, 10)}));
}

TEST(ReferenceBookCapacity, FullBookAcceptsCrossingOrderWithRemainder) {
    Harness h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(sell(2, 1, 10, 10));
    h.send(sell(2, 2, 10, 20));
    // Crosses: uses up order 1, which frees its slot, so the remaining 5 can rest.
    EXPECT_EQ(h.send(buy(1, 3, 15, 10)), (Events{accepted(3), trade(3, 1, 10, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{3}), Qty{5});
    EXPECT_EQ(h.book.order_count(), 2U);
}

TEST(ReferenceBookCapacity, CancelFreesASlot) {
    Harness h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
    h.send(buy(1, 1, 10, 10));
    EXPECT_EQ(h.send(buy(1, 2, 10, 10)), (Events{rejected(2, RejectReason::BookFull)}));
    h.send(cancel(1));
    EXPECT_EQ(h.send(buy(1, 3, 10, 10)), (Events{accepted(3)}));
}

// ---------------------------------------------------------------------------
// Market orders (design §5: any price, never rests, Rejected if nothing to trade against)
// ---------------------------------------------------------------------------

// #9
TEST_F(ReferenceBookTest, MarketOnEmptyBookIsRejected) {
    EXPECT_EQ(h.send(market_buy(1, 1, 100)), (Events{rejected(1, RejectReason::NoLiquidity)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, MarketWithOnlySameSideOrdersIsRejected) {
    h.send(buy(2, 1, 100, 10)); // a bid is no use to a market buy
    EXPECT_EQ(h.send(market_buy(1, 2, 100)), (Events{rejected(2, RejectReason::NoLiquidity)}));
}

// #10
TEST_F(ReferenceBookTest, MarketRemainderIsCanceledNotRested) {
    h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(h.send(market_buy(1, 2, 100)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 70, CancelReason::Unfilled)}));
    EXPECT_EQ(h.book.order_count(), 0U);
    EXPECT_EQ(h.book.best_bid(), std::nullopt);
}

TEST_F(ReferenceBookTest, MarketSweepsAnyPrice) {
    h.send(sell(2, 1, 50, 10));
    h.send(sell(2, 2, 50, 1000)); // top of the price band: still taken
    EXPECT_EQ(h.send(market_buy(1, 3, 80)),
              (Events{accepted(3), trade(3, 1, 50, 10), trade(3, 2, 30, 1000)}));
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{20});
}

TEST_F(ReferenceBookTest, MarketSellTakesBidsHighestFirst) {
    h.send(buy(2, 1, 40, 1));
    h.send(buy(2, 2, 40, 10));
    EXPECT_EQ(h.send(market_sell(1, 3, 50)),
              (Events{accepted(3), trade(3, 2, 40, 10), trade(3, 1, 10, 1)}));
}

TEST_F(ReferenceBookTest, MarketPriceFieldIsIgnored) {
    h.send(sell(2, 1, 10, 10));
    NewOrder m = market_buy(1, 2, 10);
    m.price = Price{-999}; // outside the band, but a Market order has no price
    EXPECT_EQ(h.send(m), (Events{accepted(2), trade(2, 1, 10, 10)}));
}

TEST_F(ReferenceBookTest, MarketBadQtyIsReportedBeforeNoLiquidity) {
    EXPECT_EQ(h.send(market_buy(1, 1, 0)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// IOC (design §5: like a Limit order, but the remainder is canceled instead of resting)
// ---------------------------------------------------------------------------

// #11
TEST_F(ReferenceBookTest, IocOnEmptyBookIsCanceled) {
    EXPECT_EQ(h.send(ioc_buy(1, 1, 100, 10)),
              (Events{accepted(1), canceled(1, 100, CancelReason::Unfilled)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, IocPartialFillCancelsRemainder) {
    h.send(sell(2, 1, 30, 10));
    EXPECT_EQ(h.send(ioc_buy(1, 2, 100, 10)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 70, CancelReason::Unfilled)}));
    EXPECT_EQ(h.book.best_bid(), std::nullopt); // nothing rested
}

TEST_F(ReferenceBookTest, IocRespectsItsLimitPrice) {
    h.send(sell(2, 1, 50, 10));
    h.send(sell(2, 2, 50, 12)); // above the IOC's limit of 11
    EXPECT_EQ(h.send(ioc_buy(1, 3, 100, 11)),
              (Events{accepted(3), trade(3, 1, 50, 10), canceled(3, 50, CancelReason::Unfilled)}));
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{50}); // untouched
}

TEST_F(ReferenceBookTest, IocFullFillHasNoCancel) {
    h.send(sell(2, 1, 100, 10));
    EXPECT_EQ(h.send(ioc_buy(1, 2, 100, 10)), (Events{accepted(2), trade(2, 1, 100, 10)}));
}

TEST_F(ReferenceBookTest, IocSellTakesBids) {
    h.send(buy(2, 1, 30, 10));
    EXPECT_EQ(h.send(ioc_sell(1, 2, 50, 10)),
              (Events{accepted(2), trade(2, 1, 30, 10), canceled(2, 20, CancelReason::Unfilled)}));
}

TEST_F(ReferenceBookTest, IocPriceOutsideBandIsInvalid) {
    EXPECT_EQ(h.send(ioc_buy(1, 1, 10, 0)), (Events{rejected(1, RejectReason::InvalidPrice)}));
}

// ---------------------------------------------------------------------------
// FOK (design §5: look first, then fill completely or kill; the book is never half-changed)
// ---------------------------------------------------------------------------

// #12
TEST_F(ReferenceBookTest, FokWithoutEnoughQtyIsKilledAndBookUnchanged) {
    h.send(sell(2, 1, 60, 10));
    EXPECT_EQ(h.send(fok_buy(1, 2, 100, 10)),
              (Events{accepted(2), canceled(2, 100, CancelReason::FokKill)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{60}); // nothing traded
}

// #13
TEST_F(ReferenceBookTest, FokWithExactlyEnoughFillsCompletely) {
    h.send(sell(2, 1, 60, 10));
    h.send(sell(2, 2, 40, 10));
    EXPECT_EQ(h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), trade(3, 1, 60, 10), trade(3, 2, 40, 10)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, FokOnlyCountsAcceptablePrices) {
    h.send(sell(2, 1, 60, 10));
    h.send(sell(2, 2, 40, 11)); // enough in total, but 11 is above the FOK's limit
    EXPECT_EQ(h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), canceled(3, 100, CancelReason::FokKill)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{60});
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{40});
}

TEST_F(ReferenceBookTest, FokFillsAcrossLevelsWithinItsLimit) {
    h.send(sell(2, 1, 60, 10));
    h.send(sell(2, 2, 40, 11));
    EXPECT_EQ(h.send(fok_buy(1, 3, 100, 11)),
              (Events{accepted(3), trade(3, 1, 60, 10), trade(3, 2, 40, 11)}));
}

TEST_F(ReferenceBookTest, FokSellAndEmptyBook) {
    EXPECT_EQ(h.send(fok_sell(1, 1, 10, 10)),
              (Events{accepted(1), canceled(1, 10, CancelReason::FokKill)}));
    h.send(buy(2, 2, 15, 10));
    EXPECT_EQ(h.send(fok_sell(1, 3, 10, 10)), (Events{accepted(3), trade(3, 2, 10, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{5});
}

// ---------------------------------------------------------------------------
// Only Limit orders rest, so only Limit orders can be BookFull
// ---------------------------------------------------------------------------

TEST(ReferenceBookCapacity, NonRestingTypesAreNeverBookFull) {
    Harness h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
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
TEST_F(ReferenceBookTest, SelfMatchCancelsOwnRestingOrderThenTrades) {
    h.send(sell(1, 1, 50, 10)); // owner 1's own order, first in the queue
    h.send(sell(2, 2, 50, 10));
    EXPECT_EQ(h.send(buy(1, 3, 50, 10)),
              (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 50, 10)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

// #15
TEST_F(ReferenceBookTest, MarketAgainstOnlyOwnOrdersIsRejectedAndOwnOrdersKept) {
    h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(h.send(market_buy(1, 2, 50)), (Events{rejected(2, RejectReason::NoLiquidity)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{50}); // not canceled
}

// #16
TEST_F(ReferenceBookTest, FokDoesNotCountOwnOrders) {
    h.send(sell(1, 1, 50, 10));
    h.send(sell(2, 2, 50, 10)); // 100 in total, but only 50 from other owners
    EXPECT_EQ(h.send(fok_buy(1, 3, 100, 10)),
              (Events{accepted(3), canceled(3, 100, CancelReason::FokKill)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{50}); // killed FOK touches nothing, own order too
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{50});
}

TEST_F(ReferenceBookTest, FokCancelsOwnOrdersItPassesWhenEnoughOthers) {
    h.send(sell(1, 1, 50, 10));  // own, better price
    h.send(sell(2, 2, 100, 11)); // enough from another owner within the limit
    EXPECT_EQ(
        h.send(fok_buy(1, 3, 100, 11)),
        (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 100, 11)}));
}

TEST_F(ReferenceBookTest, MarketCancelsOwnOrdersOnTheWay) {
    h.send(sell(1, 1, 50, 10));
    h.send(sell(2, 2, 50, 11));
    EXPECT_EQ(h.send(market_buy(1, 3, 50)),
              (Events{accepted(3), canceled(1, 50, CancelReason::SelfMatch), trade(3, 2, 50, 11)}));
}

TEST_F(ReferenceBookTest, IocAgainstOnlyOwnOrders) {
    h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(h.send(ioc_buy(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch),
                      canceled(2, 50, CancelReason::Unfilled)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, LimitAgainstOnlyOwnOrdersRestsAfterCancelingThem) {
    h.send(sell(1, 1, 50, 10));
    EXPECT_EQ(h.send(buy(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.best_bid(), Price{10}); // order 2 rests; the book is not crossed
    EXPECT_EQ(h.book.best_ask(), std::nullopt);
}

TEST_F(ReferenceBookTest, SelfMatchReportsOnlyRemainingQty) {
    h.send(sell(1, 1, 100, 10));
    h.send(buy(2, 2, 40, 10)); // another owner takes 40 of order 1
    EXPECT_EQ(h.send(buy(1, 3, 10, 10)),
              (Events{accepted(3), canceled(1, 60, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.leaves(OrderId{3}), Qty{10});
}

TEST_F(ReferenceBookTest, OwnOrdersBeyondTheFillAreUntouched) {
    h.send(sell(2, 1, 50, 10));
    h.send(sell(1, 2, 50, 10)); // own order, but behind order 1
    EXPECT_EQ(h.send(buy(1, 3, 50, 10)), (Events{accepted(3), trade(3, 1, 50, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{2}), Qty{50}); // never reached, so not canceled
}

TEST_F(ReferenceBookTest, SelfMatchOnSellSide) {
    h.send(buy(1, 1, 50, 10));
    EXPECT_EQ(h.send(sell(1, 2, 50, 10)),
              (Events{accepted(2), canceled(1, 50, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.best_ask(), Price{10});
}

TEST(ReferenceBookCapacity, CrossingOnlyOwnOrderOnFullBookFreesTheSlot) {
    Harness h{BookConfig{Price{1}, Price{1000}, 1, Qty{1'000'000}}};
    h.send(sell(1, 1, 10, 10)); // book is full
    EXPECT_EQ(h.send(buy(1, 2, 10, 10)),
              (Events{accepted(2), canceled(1, 10, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.order_count(), 1U); // order 2 took the freed slot
}

// ---------------------------------------------------------------------------
// Modify (design §7). Helper order: modify(id, new TOTAL qty, new price).
// ---------------------------------------------------------------------------

// #17
TEST_F(ReferenceBookTest, ModifyDownKeepsPlaceInQueue) {
    h.send(buy(1, 1, 100, 10));
    h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(h.send(modify(1, 60, 10)), (Events{replaced(1, 60, 10)}));
    EXPECT_EQ(h.book.level_orders(Side::Buy, Price{10}), (std::vector{OrderId{1}, OrderId{2}}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{60});
}

// #18
TEST_F(ReferenceBookTest, ModifyUpLosesPlaceInQueue) {
    h.send(buy(1, 1, 100, 10));
    h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(h.send(modify(1, 150, 10)), (Events{replaced(1, 150, 10)}));
    EXPECT_EQ(h.book.level_orders(Side::Buy, Price{10}), (std::vector{OrderId{2}, OrderId{1}}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{150});
}

// #19: the race from design §7. 30 traded before the modify arrived, so total 50 leaves 20.
TEST_F(ReferenceBookTest, ModifyTotalCountsWhatAlreadyTraded) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 30, 10)); // 30 of order 1 trade
    EXPECT_EQ(h.send(modify(1, 50, 10)), (Events{replaced(1, 50, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{20}); // not 50: no overfill
}

TEST_F(ReferenceBookTest, ModifyUpCountsWhatAlreadyTraded) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 30, 10)); // 30 of order 1 trade
    EXPECT_EQ(h.send(modify(1, 150, 10)), (Events{replaced(1, 150, 10)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{120}); // 150 total - 30 traded, not 150
}

// #20
TEST_F(ReferenceBookTest, ModifyTotalAtOrBelowFilledIsTooLate) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 30, 10));
    EXPECT_EQ(h.send(modify(1, 30, 10)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
    EXPECT_EQ(h.send(modify(1, 20, 10)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{70}); // unchanged
}

// #21
TEST_F(ReferenceBookTest, ModifyToZeroMustUseCancel) {
    h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(h.send(modify(1, 0, 10)), (Events{rejected(1, RejectReason::UseCancel)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{100});
}

// #22
TEST_F(ReferenceBookTest, ModifyToCrossingPriceTradesImmediately) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 100, 12));
    EXPECT_EQ(h.send(modify(1, 100, 12)), (Events{replaced(1, 100, 12), trade(1, 2, 100, 12)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

// #25
TEST_F(ReferenceBookTest, ModifyThatChangesNothingIsRejected) {
    h.send(buy(1, 1, 100, 10));
    h.send(buy(2, 2, 100, 10));
    EXPECT_EQ(h.send(modify(1, 100, 10)), (Events{rejected(1, RejectReason::NoChange)}));
    EXPECT_EQ(h.book.level_orders(Side::Buy, Price{10}), (std::vector{OrderId{1}, OrderId{2}}));
}

TEST_F(ReferenceBookTest, ModifyOfNeverSentIdIsUnknown) {
    EXPECT_EQ(h.send(modify(9, 10, 10)), (Events{rejected(9, RejectReason::UnknownOrder)}));
}

// A book only knows resting orders; the exchange refines these to TooLateToCancel (E9).
TEST_F(ReferenceBookTest, ModifyOfFinishedOrderIsUnknownToTheBook) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 100, 10)); // order 1 fully filled
    h.send(buy(1, 3, 100, 10));
    h.send(cancel(3)); // order 3 canceled
    EXPECT_EQ(h.send(modify(1, 50, 10)), (Events{rejected(1, RejectReason::UnknownOrder)}));
    EXPECT_EQ(h.send(modify(3, 50, 10)), (Events{rejected(3, RejectReason::UnknownOrder)}));
}

TEST_F(ReferenceBookTest, ModifyOfIocIsUnknownToTheBook) {
    h.send(ioc_buy(1, 1, 100, 10)); // never rests, finished straight away
    EXPECT_EQ(h.send(modify(1, 50, 10)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TEST_F(ReferenceBookTest, ModifyInvalidQtyAndPrice) {
    h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(h.send(modify(1, -5, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.send(modify(1, 50, 0)), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(h.send(modify(1, 50, 1001)), (Events{rejected(1, RejectReason::InvalidPrice)}));
}

TEST_F(ReferenceBookTest, ModifyChecksRunInContractOrder) {
    h.send(buy(1, 1, 100, 10));
    // Total 0 AND bad price: UseCancel (step 3) comes before InvalidPrice (step 4).
    EXPECT_EQ(h.send(modify(1, 0, 0)), (Events{rejected(1, RejectReason::UseCancel)}));
    // Negative total AND bad price: InvalidQty comes before InvalidPrice.
    EXPECT_EQ(h.send(modify(1, -1, 0)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

TEST_F(ReferenceBookTest, ModifyPriceLosesPlaceEvenWhenShrinking) {
    h.send(buy(1, 1, 100, 10));
    h.send(buy(2, 2, 100, 9));
    EXPECT_EQ(h.send(modify(1, 50, 9)), (Events{replaced(1, 50, 9)}));
    EXPECT_EQ(h.book.level_orders(Side::Buy, Price{9}), (std::vector{OrderId{2}, OrderId{1}}));
    EXPECT_EQ(h.book.best_bid(), Price{9}); // level 10 emptied and is gone
}

TEST_F(ReferenceBookTest, ModifyCrossingRemainderRestsAtNewPrice) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 40, 12));
    EXPECT_EQ(h.send(modify(1, 100, 12)), (Events{replaced(1, 100, 12), trade(1, 2, 40, 12)}));
    EXPECT_EQ(h.book.best_bid(), Price{12});
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{60});
}

TEST_F(ReferenceBookTest, ModifyCrossingOnlyTradesWhatIsLeft) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(2, 2, 30, 10));  // 30 of order 1 trade
    h.send(sell(3, 3, 100, 12)); // plenty available at 12
    // Total 100 with 30 already traded: only 70 more may trade.
    EXPECT_EQ(h.send(modify(1, 100, 12)), (Events{replaced(1, 100, 12), trade(1, 3, 70, 12)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), std::nullopt); // complete
    EXPECT_EQ(h.book.leaves(OrderId{3}), Qty{30});
}

TEST_F(ReferenceBookTest, ModifyCrossingOwnOrderTriggersSelfMatch) {
    h.send(buy(1, 1, 100, 10));
    h.send(sell(1, 2, 100, 12)); // same owner
    EXPECT_EQ(h.send(modify(1, 100, 12)),
              (Events{replaced(1, 100, 12), canceled(2, 100, CancelReason::SelfMatch)}));
    EXPECT_EQ(h.book.best_bid(), Price{12});
    EXPECT_EQ(h.book.best_ask(), std::nullopt);
}

TEST_F(ReferenceBookTest, ModifySellSideKeepsAndLosesPlace) {
    h.send(sell(1, 1, 100, 10));
    h.send(sell(2, 2, 100, 10));
    h.send(modify(1, 40, 10)); // down: keeps place
    EXPECT_EQ(h.book.level_orders(Side::Sell, Price{10}), (std::vector{OrderId{1}, OrderId{2}}));
    h.send(modify(1, 80, 10)); // up: loses place
    EXPECT_EQ(h.book.level_orders(Side::Sell, Price{10}), (std::vector{OrderId{2}, OrderId{1}}));
}

TEST_F(ReferenceBookTest, ModifiedOrderCanBeModifiedAndCanceledAgain) {
    h.send(buy(1, 1, 100, 10));
    h.send(modify(1, 120, 11));
    h.send(modify(1, 90, 11));
    EXPECT_EQ(h.send(cancel(1)), (Events{canceled(1, 90, CancelReason::Requested)}));
    EXPECT_EQ(h.book.order_count(), 0U);
}

TEST(ReferenceBookCapacity, ModifyIsNeverBookFull) {
    Harness h{BookConfig{Price{1}, Price{1000}, 2, Qty{1'000'000}}};
    h.send(buy(1, 1, 10, 10));
    h.send(buy(1, 2, 10, 9));                                          // book is full
    EXPECT_EQ(h.send(modify(1, 20, 8)), (Events{replaced(1, 20, 8)})); // frees its own slot first
    EXPECT_EQ(h.book.order_count(), 2U);
}

// ---------------------------------------------------------------------------
// Quantity limit (design §12: max_qty keeps every sum inside 64 bits)
// ---------------------------------------------------------------------------

// #32
TEST_F(ReferenceBookTest, QtyAboveMaxIsInvalid) {
    EXPECT_EQ(h.send(buy(1, 1, 1'000'001, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.send(buy(1, 2, 1'000'000, 10)), (Events{accepted(2)})); // exactly max: allowed
}

TEST_F(ReferenceBookTest, ModifyTotalAboveMaxIsInvalid) {
    h.send(buy(1, 1, 100, 10));
    EXPECT_EQ(h.send(modify(1, 1'000'001, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.book.leaves(OrderId{1}), Qty{100});
}

// The overflow found in review: two huge orders summed past 2^63 inside the FOK look-first count.
// A config that allows that is now invalid, and with a valid config such orders are rejected.
TEST(ReferenceBookQtyLimit, HugeQuantitiesCannotOverflow) {
    constexpr std::int64_t big = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    EXPECT_FALSE((BookConfig{Price{1}, Price{1000}, 10, Qty{big}}.is_valid()));

    Harness h{kConfig};
    EXPECT_EQ(h.send(sell(2, 1, big, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------------------
// Found in review: correct already, now locked in by tests
// ---------------------------------------------------------------------------

TEST_F(ReferenceBookTest, SelfMatchInMiddleOfQueue) {
    h.send(sell(2, 1, 10, 10));
    h.send(sell(1, 2, 10, 10)); // own order, between two others
    h.send(sell(3, 3, 10, 10));
    EXPECT_EQ(h.send(buy(1, 4, 20, 10)),
              (Events{accepted(4), trade(4, 1, 10, 10), canceled(2, 10, CancelReason::SelfMatch),
                      trade(4, 3, 10, 10)}));
}

// One input can produce max_orders + 2 events (design §12). Build that exact worst case, so the
// EventBuffer's assert would fire if the formula were ever too small.
TEST(ReferenceBookCapacity, WorstCaseEventCountFitsBuffer) {
    constexpr BookConfig cfg{Price{1}, Price{1000}, 50, Qty{1'000'000}};
    Harness h{cfg};
    for (std::int64_t i = 1; i <= 50; ++i) {
        h.send(sell(2, static_cast<std::uint64_t>(i), 1, i)); // 50 orders on 50 levels: full
    }
    const Events events = h.send(ioc_buy(1, 51, 1000, 1000)); // New + 50 trades + Canceled
    EXPECT_EQ(events.size(), cfg.max_events_per_input());
    EXPECT_EQ(events.back(), canceled(51, 950, CancelReason::Unfilled));
}

#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include <cstdint>

// Test numbers E1-E9 refer to the exchange rows of the design §10 table. Two symbols: A and M.
// Ids in the expected events are the ones the exchange assigns: 1, 2, 3, ... for all symbols.

using namespace nanobook;
using namespace nanobook::test;
using Events = std::vector<Event>;

namespace {

constexpr SymbolId A{0};
constexpr SymbolId M{1};
constexpr SymbolId Unlisted{2};
constexpr BookConfig kBook{Price{1}, Price{1000}, 100, Qty{1'000'000}};

ExchangeConfig config(std::uint64_t max_orders_per_session = 100) {
    return ExchangeConfig{.max_orders_per_session = max_orders_per_session,
                          .books = {kBook, kBook}};
}

class ExchangeTest : public ::testing::Test {
protected:
    ExchangeHarness h{config()};
};

} // namespace

// ---------------------------------------------------------------------------
// One counter for the whole exchange
// ---------------------------------------------------------------------------

// E1
TEST_F(ExchangeTest, OneCounterForAllSymbols) {
    EXPECT_EQ(h.send(req_buy(A, 1, 10, 10)), (Events{accepted(1)}));
    EXPECT_EQ(h.send(req_buy(M, 1, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(h.send(req_buy(A, 1, 10, 9)), (Events{accepted(3)}));
    EXPECT_EQ(h.exchange.book(A).resting_ids(), (std::vector{OrderId{1}, OrderId{3}}));
    EXPECT_EQ(h.exchange.book(M).resting_ids(), (std::vector{OrderId{2}}));
    EXPECT_EQ(h.exchange.next_id(), OrderId{4});
}

// E2
TEST_F(ExchangeTest, RejectedOrderUsesUpItsId) {
    EXPECT_EQ(h.send(req_buy(A, 1, 0, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(h.send(req_buy(A, 1, 10, 10)), (Events{accepted(2)}));
}

// E3
TEST_F(ExchangeTest, UnlistedSymbolIsRejected) {
    EXPECT_EQ(h.send(req_buy(Unlisted, 1, 10, 10)),
              (Events{rejected(1, RejectReason::UnknownSymbol)}));
    EXPECT_EQ(h.exchange.next_id(), OrderId{2}); // id 1 was used up
}

TEST_F(ExchangeTest, SymbolsAreSeparateBooks) {
    h.send(req_sell(A, 2, 10, 10));
    // Same price on another symbol: nothing to match against there, so it rests.
    EXPECT_EQ(h.send(req_buy(M, 1, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(h.exchange.book(A).best_ask(), Price{10});
    EXPECT_EQ(h.exchange.book(M).best_bid(), Price{10});
}

TEST_F(ExchangeTest, MatchingWorksThroughTheExchange) {
    h.send(req_sell(A, 2, 100, 10));                                                      // id 1
    EXPECT_EQ(h.send(req_buy(A, 1, 40, 10)), (Events{accepted(2), trade(2, 1, 40, 10)})); // id 2
    EXPECT_EQ(h.exchange.book(A).leaves(OrderId{1}), Qty{60});
}

// ---------------------------------------------------------------------------
// Cancel and Modify: exact reasons (design §3.1)
// ---------------------------------------------------------------------------

TEST_F(ExchangeTest, CancelAndModifyOfRestingOrderWork) {
    h.send(req_buy(A, 1, 100, 10)); // id 1
    EXPECT_EQ(h.send(req_modify(A, 1, 60, 10)), (Events{replaced(1, 60, 10)}));
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{canceled(1, 60, CancelReason::Requested)}));
}

// E4
TEST_F(ExchangeTest, NeverIssuedIdIsUnknown) {
    for (int i = 0; i < 7; ++i) {
        h.send(req_buy(A, 1, 10, 10)); // ids 1-7
    }
    EXPECT_EQ(h.send(req_cancel(A, 8)), (Events{rejected(8, RejectReason::UnknownOrder)}));
    EXPECT_EQ(h.send(req_cancel(A, 0)), (Events{rejected(0, RejectReason::UnknownOrder)}));
    EXPECT_EQ(h.send(req_modify(A, 8, 5, 10)), (Events{rejected(8, RejectReason::UnknownOrder)}));
}

// E5
TEST_F(ExchangeTest, CancelOfFinishedOrderIsTooLate) {
    h.send(req_sell(A, 2, 100, 10)); // id 1
    h.send(req_buy(A, 1, 100, 10));  // id 2: fills id 1 completely
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TEST_F(ExchangeTest, CancelTwiceIsTooLate) {
    h.send(req_buy(A, 1, 100, 10)); // id 1
    h.send(req_cancel(A, 1));
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TEST_F(ExchangeTest, CancelOfFinishedIocIsTooLate) {
    h.send(request(A, OrderType::IOC, Side::Buy, 1, 100, 10)); // id 1: New, then Canceled
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

// E6
TEST_F(ExchangeTest, CancelOnWrongSymbolIsUnknownAndHarmless) {
    h.send(req_buy(A, 1, 100, 10)); // id 1, in A
    EXPECT_EQ(h.send(req_cancel(M, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
    EXPECT_EQ(h.exchange.book(A).leaves(OrderId{1}), Qty{100}); // untouched
}

// E7
TEST_F(ExchangeTest, CancelOfRejectedOrderIsUnknown) {
    h.send(req_buy(A, 1, 0, 10)); // id 1: rejected, never entered a book
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TEST_F(ExchangeTest, CancelOfUnlistedSymbolIdIsUnknown) {
    h.send(req_buy(Unlisted, 1, 10, 10)); // id 1: rejected UnknownSymbol
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TEST_F(ExchangeTest, CancelOrModifyOnUnlistedSymbol) {
    h.send(req_buy(A, 1, 10, 10)); // id 1
    EXPECT_EQ(h.send(req_cancel(Unlisted, 1)), (Events{rejected(1, RejectReason::UnknownSymbol)}));
    EXPECT_EQ(h.send(req_modify(Unlisted, 1, 5, 10)),
              (Events{rejected(1, RejectReason::UnknownSymbol)}));
}

// E9
TEST_F(ExchangeTest, ModifyOfFinishedOrderIsTooLate) {
    h.send(req_sell(A, 2, 100, 10)); // id 1
    h.send(req_buy(A, 1, 100, 10));  // id 2: fills id 1 completely
    EXPECT_EQ(h.send(req_modify(A, 1, 50, 10)),
              (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TEST_F(ExchangeTest, BookLevelModifyRejectsPassThroughUnchanged) {
    h.send(req_buy(A, 1, 100, 10)); // id 1
    EXPECT_EQ(h.send(req_modify(A, 1, 0, 10)), (Events{rejected(1, RejectReason::UseCancel)}));
    EXPECT_EQ(h.send(req_modify(A, 1, 100, 10)), (Events{rejected(1, RejectReason::NoChange)}));
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

// E8
TEST(ExchangeCapacity, IdsRunOut) {
    ExchangeHarness h{config(2)};
    EXPECT_EQ(h.send(req_buy(A, 1, 10, 10)), (Events{accepted(1)}));
    EXPECT_EQ(h.send(req_buy(M, 1, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(h.send(req_buy(A, 1, 10, 10)), (Events{rejected(0, RejectReason::IdsExhausted)}));
    // Cancels still work: they don't need a new id.
    EXPECT_EQ(h.send(req_cancel(A, 1)), (Events{canceled(1, 10, CancelReason::Requested)}));
}

TEST(ExchangeConfigTest, Validity) {
    EXPECT_TRUE(config().is_valid());
    EXPECT_FALSE(config(0).is_valid()); // no ids at all
    EXPECT_FALSE((ExchangeConfig{.max_orders_per_session = 10, .books = {}}.is_valid()));
    BookConfig bad = kBook;
    bad.max_qty = Qty{0};
    EXPECT_FALSE((ExchangeConfig{.max_orders_per_session = 10, .books = {kBook, bad}}.is_valid()));
}

TEST(ExchangeConfigTest, BufferFitsTheBiggestBook) {
    const BookConfig big{Price{1}, Price{1000}, 500, Qty{1'000'000}};
    const ExchangeConfig cfg{.max_orders_per_session = 10, .books = {kBook, big}};
    EXPECT_EQ(cfg.max_events_per_input(), big.max_events_per_input());
}

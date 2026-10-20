/*
 * test_exchange.cpp: tests for the exchange layer.
 *
 * What it does: Checks the exchange-wide id counter (ids 1, 2, 3 across stocks), routing to the
 *   right book, and the exact reasons for cancels and modifies (rows E1 to E9 of the design §10
 *   table): never issued, finished, wrong stock, rejected, and ids running out.
 *
 * How it works: Typed tests over the book type, with two stocks, A and M. Ids in the expected
 *   events are the ones the exchange assigns.
 */

#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include <cstdint>

using namespace nanobook;
using namespace nanobook::test;
using Events = std::vector<Event>;

namespace {

constexpr SymbolId A{0};
constexpr SymbolId M{1};
constexpr SymbolId Unlisted{2};
constexpr BookConfig kBook{
    .min_price = Price{1}, .max_price = Price{1000}, .max_orders = 100, .max_qty = Qty{1'000'000}};

ExchangeConfig config(std::uint64_t max_orders_per_session = 100) {
    return ExchangeConfig{.max_orders_per_session = max_orders_per_session,
                          .books = {kBook, kBook}};
}

// Runs every test below once per type in BookTypes, as the books inside the exchange.
template <typename Book> class ExchangeTest : public ::testing::Test {
protected:
    ExchangeHarness<Book> h{config()};
};

template <typename Book> class ExchangeCapacityTest : public ::testing::Test {};

} // namespace

TYPED_TEST_SUITE(ExchangeTest, BookTypes, BookTypeNames);
TYPED_TEST_SUITE(ExchangeCapacityTest, BookTypes, BookTypeNames);

// ---------------------------------------------------------------------------
// One counter for the whole exchange
// ---------------------------------------------------------------------------

// E1
TYPED_TEST(ExchangeTest, OneCounterForAllSymbols) {
    EXPECT_EQ(this->h.send(req_buy(A, 1, 10, 10)), (Events{accepted(1)}));
    EXPECT_EQ(this->h.send(req_buy(M, 1, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(this->h.send(req_buy(A, 1, 10, 9)), (Events{accepted(3)}));
    EXPECT_EQ(this->h.exchange.book(A).resting_ids(), (std::vector{OrderId{1}, OrderId{3}}));
    EXPECT_EQ(this->h.exchange.book(M).resting_ids(), (std::vector{OrderId{2}}));
    EXPECT_EQ(this->h.exchange.next_id(), OrderId{4});
}

// E2
TYPED_TEST(ExchangeTest, RejectedOrderUsesUpItsId) {
    EXPECT_EQ(this->h.send(req_buy(A, 1, 0, 10)), (Events{rejected(1, RejectReason::InvalidQty)}));
    EXPECT_EQ(this->h.send(req_buy(A, 1, 10, 10)), (Events{accepted(2)}));
}

// E3
TYPED_TEST(ExchangeTest, UnlistedSymbolIsRejected) {
    EXPECT_EQ(this->h.send(req_buy(Unlisted, 1, 10, 10)),
              (Events{rejected(1, RejectReason::UnknownSymbol)}));
    EXPECT_EQ(this->h.exchange.next_id(), OrderId{2}); // id 1 was used up
}

TYPED_TEST(ExchangeTest, SymbolsAreSeparateBooks) {
    this->h.send(req_sell(A, 2, 10, 10));
    // Same price on another symbol: nothing to match against there, so it rests.
    EXPECT_EQ(this->h.send(req_buy(M, 1, 10, 10)), (Events{accepted(2)}));
    EXPECT_EQ(this->h.exchange.book(A).best_ask(), Price{10});
    EXPECT_EQ(this->h.exchange.book(M).best_bid(), Price{10});
}

TYPED_TEST(ExchangeTest, MatchingWorksThroughTheExchange) {
    this->h.send(req_sell(A, 2, 100, 10)); // id 1
    EXPECT_EQ(this->h.send(req_buy(A, 1, 40, 10)),
              (Events{accepted(2), trade(2, 1, 40, 10)})); // id 2
    EXPECT_EQ(this->h.exchange.book(A).leaves(OrderId{1}), Qty{60});
}

// ---------------------------------------------------------------------------
// Cancel and Modify: exact reasons (design §3.1)
// ---------------------------------------------------------------------------

TYPED_TEST(ExchangeTest, CancelAndModifyOfRestingOrderWork) {
    this->h.send(req_buy(A, 1, 100, 10)); // id 1
    EXPECT_EQ(this->h.send(req_modify(A, 1, 60, 10)), (Events{replaced(1, 60, 10)}));
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{canceled(1, 60, CancelReason::Requested)}));
}

// E4
TYPED_TEST(ExchangeTest, NeverIssuedIdIsUnknown) {
    for (int i = 0; i < 7; ++i) {
        this->h.send(req_buy(A, 1, 10, 10)); // ids 1-7
    }
    EXPECT_EQ(this->h.send(req_cancel(A, 8)), (Events{rejected(8, RejectReason::UnknownOrder)}));
    EXPECT_EQ(this->h.send(req_cancel(A, 0)), (Events{rejected(0, RejectReason::UnknownOrder)}));
    EXPECT_EQ(this->h.send(req_modify(A, 8, 5, 10)),
              (Events{rejected(8, RejectReason::UnknownOrder)}));
}

// E5
TYPED_TEST(ExchangeTest, CancelOfFinishedOrderIsTooLate) {
    this->h.send(req_sell(A, 2, 100, 10)); // id 1
    this->h.send(req_buy(A, 1, 100, 10));  // id 2: fills id 1 completely
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TYPED_TEST(ExchangeTest, CancelTwiceIsTooLate) {
    this->h.send(req_buy(A, 1, 100, 10)); // id 1
    this->h.send(req_cancel(A, 1));
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TYPED_TEST(ExchangeTest, CancelOfFinishedIocIsTooLate) {
    this->h.send(request(A, OrderType::IOC, Side::Buy, 1, 100, 10)); // id 1: New, then Canceled
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

// E6
TYPED_TEST(ExchangeTest, CancelOnWrongSymbolIsUnknownAndHarmless) {
    this->h.send(req_buy(A, 1, 100, 10)); // id 1, in A
    EXPECT_EQ(this->h.send(req_cancel(M, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
    EXPECT_EQ(this->h.exchange.book(A).leaves(OrderId{1}), Qty{100}); // untouched
}

// E7
TYPED_TEST(ExchangeTest, CancelOfRejectedOrderIsUnknown) {
    this->h.send(req_buy(A, 1, 0, 10)); // id 1: rejected, never entered a book
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(ExchangeTest, CancelOfUnlistedSymbolIdIsUnknown) {
    this->h.send(req_buy(Unlisted, 1, 10, 10)); // id 1: rejected UnknownSymbol
    EXPECT_EQ(this->h.send(req_cancel(A, 1)), (Events{rejected(1, RejectReason::UnknownOrder)}));
}

TYPED_TEST(ExchangeTest, CancelOrModifyOnUnlistedSymbol) {
    this->h.send(req_buy(A, 1, 10, 10)); // id 1
    EXPECT_EQ(this->h.send(req_cancel(Unlisted, 1)),
              (Events{rejected(1, RejectReason::UnknownSymbol)}));
    EXPECT_EQ(this->h.send(req_modify(Unlisted, 1, 5, 10)),
              (Events{rejected(1, RejectReason::UnknownSymbol)}));
}

// E9
TYPED_TEST(ExchangeTest, ModifyOfFinishedOrderIsTooLate) {
    this->h.send(req_sell(A, 2, 100, 10)); // id 1
    this->h.send(req_buy(A, 1, 100, 10));  // id 2: fills id 1 completely
    EXPECT_EQ(this->h.send(req_modify(A, 1, 50, 10)),
              (Events{rejected(1, RejectReason::TooLateToCancel)}));
}

TYPED_TEST(ExchangeTest, BookLevelModifyRejectsPassThroughUnchanged) {
    this->h.send(req_buy(A, 1, 100, 10)); // id 1
    EXPECT_EQ(this->h.send(req_modify(A, 1, 0, 10)),
              (Events{rejected(1, RejectReason::UseCancel)}));
    EXPECT_EQ(this->h.send(req_modify(A, 1, 100, 10)),
              (Events{rejected(1, RejectReason::NoChange)}));
}

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

// E8
TYPED_TEST(ExchangeCapacityTest, IdsRunOut) {
    ExchangeHarness<TypeParam> h{config(2)};
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
    const BookConfig big{.min_price = Price{1},
                         .max_price = Price{1000},
                         .max_orders = 500,
                         .max_qty = Qty{1'000'000}};
    const ExchangeConfig cfg{.max_orders_per_session = 10, .books = {kBook, big}};
    EXPECT_EQ(cfg.max_events_per_input(), big.max_events_per_input());
}

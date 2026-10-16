/*
 * test_messages.cpp: tests for events.
 *
 * What it does: Checks that each make_* function sets exactly its own fields and leaves the rest at
 *   0, and that == notices a difference in any field.
 *
 * Why it exists: The differential fuzzer will compare whole event streams with ==, so == has to
 *   look at everything.
 */

#include <gtest/gtest.h>
#include "nanobook/core/messages.hpp"

using namespace nanobook;

TEST(Event, MakeTradeSetsOnlyTradeFields) {
    const Event e = Event::make_trade(OrderId{2}, OrderId{1}, Price{10}, Qty{40});
    EXPECT_EQ(e.type, EventType::Trade);
    EXPECT_EQ(e.id, OrderId{2});
    EXPECT_EQ(e.resting_id, OrderId{1});
    EXPECT_EQ(e.price, Price{10});
    EXPECT_EQ(e.qty, Qty{40});
    EXPECT_EQ(e.cancel_reason, CancelReason::None);
    EXPECT_EQ(e.reject_reason, RejectReason::None);
}

TEST(Event, MakeCanceledAndRejected) {
    const Event c = Event::make_canceled(OrderId{2}, Qty{70}, CancelReason::Unfilled);
    EXPECT_EQ(c.type, EventType::Canceled);
    EXPECT_EQ(c.qty, Qty{70});
    EXPECT_EQ(c.cancel_reason, CancelReason::Unfilled);
    EXPECT_EQ(c.resting_id, OrderId{}); // unused field stays zero

    const Event r = Event::make_rejected(OrderId{9}, RejectReason::UnknownOrder);
    EXPECT_EQ(r.type, EventType::Rejected);
    EXPECT_EQ(r.reject_reason, RejectReason::UnknownOrder);
    EXPECT_EQ(r.qty, Qty{});
}

// The phase 2 differential test compares event streams with ==, so == must look at every field.
TEST(Event, EqualityComparesEveryField) {
    const Event a = Event::make_trade(OrderId{2}, OrderId{1}, Price{10}, Qty{40});
    EXPECT_EQ(a, Event::make_trade(OrderId{2}, OrderId{1}, Price{10}, Qty{40}));
    EXPECT_NE(a, Event::make_trade(OrderId{3}, OrderId{1}, Price{10}, Qty{40}));
    EXPECT_NE(a, Event::make_trade(OrderId{2}, OrderId{9}, Price{10}, Qty{40}));
    EXPECT_NE(a, Event::make_trade(OrderId{2}, OrderId{1}, Price{11}, Qty{40}));
    EXPECT_NE(a, Event::make_trade(OrderId{2}, OrderId{1}, Price{10}, Qty{41}));
    EXPECT_NE(Event::make_canceled(OrderId{1}, Qty{5}, CancelReason::Requested),
              Event::make_canceled(OrderId{1}, Qty{5}, CancelReason::SelfMatch));
    EXPECT_NE(Event::make_rejected(OrderId{1}, RejectReason::NoChange),
              Event::make_rejected(OrderId{1}, RejectReason::UseCancel));
    EXPECT_NE(Event::make_new(OrderId{1}), Event::make_rejected(OrderId{1}, RejectReason::None));
}

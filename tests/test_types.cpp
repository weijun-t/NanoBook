#include <gtest/gtest.h>
#include "nanobook/types.hpp"

using namespace nanobook;

TEST(Types, DefaultIsZero) {
    EXPECT_EQ(Price{}.ticks, 0);
    EXPECT_EQ(Qty{}.value, 0);
    EXPECT_EQ(OrderId{}.value, 0U);
    EXPECT_EQ(OwnerId{}.value, 0U);
}

TEST(Types, PriceOrdering) {
    EXPECT_LT(Price{100}, Price{101});
    EXPECT_EQ(Price{100}, Price{100});
    EXPECT_GT(Price{-1}, Price{-2});
}

TEST(Types, QtyArithmetic) {
    Qty q{100};
    q -= Qty{30};
    EXPECT_EQ(q, Qty{70});
    q += Qty{5};
    EXPECT_EQ(q, Qty{75});
    EXPECT_EQ(Qty{10} + Qty{5}, Qty{15});
    EXPECT_EQ(Qty{10} - Qty{15}, Qty{-5}); // signed: goes negative, does not wrap
}

TEST(Types, OrderIdOrdering) {
    EXPECT_LT(OrderId{7}, OrderId{8});
    EXPECT_EQ(OrderId{7}, OrderId{7});
}

TEST(Types, OwnerIdEquality) {
    EXPECT_EQ(OwnerId{1}, OwnerId{1});
    EXPECT_NE(OwnerId{1}, OwnerId{2});
}

TEST(BookConfig, Validity) {
    EXPECT_TRUE((BookConfig{Price{1}, Price{1000}, 10}.is_valid()));
    EXPECT_FALSE((BookConfig{Price{0}, Price{1000}, 10}.is_valid())); // min_price must be >= 1
    EXPECT_FALSE((BookConfig{Price{50}, Price{10}, 10}.is_valid()));  // min above max
    EXPECT_FALSE((BookConfig{Price{1}, Price{1000}, 0}.is_valid()));  // no capacity
}

TEST(BookConfig, PriceBandIsInclusive) {
    constexpr BookConfig cfg{Price{10}, Price{20}, 1};
    EXPECT_FALSE(cfg.in_band(Price{9}));
    EXPECT_TRUE(cfg.in_band(Price{10}));
    EXPECT_TRUE(cfg.in_band(Price{20}));
    EXPECT_FALSE(cfg.in_band(Price{21}));
}

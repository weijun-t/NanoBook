#include <gtest/gtest.h>
#include "nanobook/core.hpp"

TEST(CoreTest, InitializeTest) {
    nanobook::Core core;
    core.initialize();
    EXPECT_EQ(core.value(), 1);
}
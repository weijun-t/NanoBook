/*
 * test_depth_render.cpp: tests for nanobook_depth's screen layout (tools/depth_render.hpp).
 *
 * What it does: Makes frames from small replays (ITCH bytes from the test encoder), renders them
 *   into an off-screen FTXUI Screen and reads them back as text: the header (stock, clock,
 *   speed, state, best prices, spread, last trade, the check), both ladders with the best prices
 *   meeting in the middle, the selected level's queue with the shares ahead, the tape (a
 *   replace's two ids, a rejected message's result), the followed order's place, and a
 *   difference between the books. Also the small helpers (ordinal(), speed_text(), the split of
 *   the height between queue and tape), and that a full frame fits 110 columns.
 *
 * Why it exists: The layout is the part a person reads. These tests catch a column that slipped
 *   or a line that went missing without anyone having to look.
 *
 * How it works: As test_view_render.cpp: ftxui::Screen plus ftxui::Render() draw without a
 *   terminal. Built into nanobook_view_tests (it links FTXUI), only with
 *   -DNANOBOOK_BUILD_VIEWER=ON.
 */

#include <gtest/gtest.h>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <cstdint>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "depth_render.hpp" // tools/
#include "depth_replay.hpp" // support/
#include "itch_encoder.hpp"
#include "itch_golden_day.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"

using namespace nanobook;
using namespace nanobook::depth;
using namespace nanobook::itch;
using golden::add;
using golden::kAaa;
using golden::kBbb;
using golden::kNsdq;

namespace {

constexpr std::uint16_t kA = 1;
constexpr std::uint32_t kTen = 100'000; // $10.0000
constexpr int kWidth = 110;
constexpr int kHeight = 40;

// AAA, 09:30:00 onwards (t in ns since midnight): three buys at $10.00 (the second attributed
// to NSDQ), one at $9.99, two sells at $10.02 and $10.03; ref 1 executes 30; a cancel of an
// unknown order; ref 6 replaced by ref 7 at $10.04.
constexpr std::uint64_t kOpen = 34'200ULL * 1'000'000'000;

std::vector<std::byte> day() {
    Writer w;
    w.stock_directory({.locate = kA, .timestamp = 1, .stock = kAaa});
    w.stock_directory({.locate = 2, .timestamp = 1, .stock = kBbb});
    w.trading_action({.locate = kA, .timestamp = 2, .stock = kAaa, .trading_state = 'T'});
    w.add_order(add(kOpen + 1, kA, 1, 'B', 100, kTen));
    AddOrder f = add(kOpen + 2, kA, 2, 'B', 200, kTen);
    f.attribution = kNsdq;
    w.add_order_mpid(f);
    w.add_order(add(kOpen + 3, kA, 3, 'B', 300, kTen));
    w.add_order(add(kOpen + 4, kA, 4, 'B', 50, kTen - 100));
    w.add_order(add(kOpen + 5, kA, 5, 'S', 70, kTen + 200));
    w.add_order(add(kOpen + 6, kA, 6, 'S', 80, kTen + 300));
    w.executed({.locate = kA, .timestamp = kOpen + 7, .order_ref = 1, .executed_shares = 30});
    w.cancel({.locate = kA, .timestamp = kOpen + 8, .order_ref = 99, .cancelled_shares = 1});
    w.replace({.locate = kA,
               .timestamp = kOpen + 9,
               .original_order_ref = 6,
               .new_order_ref = 7,
               .shares = 80,
               .price = kTen + 400});
    const std::span<const std::byte> bytes = w.bytes();
    return {bytes.begin(), bytes.end()};
}

std::vector<BookConfig> configs(std::span<const std::byte> bytes) {
    DayCounter counter;
    EXPECT_TRUE(parse(bytes, counter).has_value());
    return one_stock_configs(counter.directory(), counter.stats(), kA);
}

std::string draw(const DepthFrame &f, int height = kHeight) {
    constexpr std::size_t kLevels = 10;
    auto screen = ftxui::Screen(kWidth, height);
    ftxui::Render(screen, render(f, kLevels, depth_rows(height, kLevels)));
    // Without the colour and inverse escape codes, so a styled line reads as plain text.
    return std::regex_replace(screen.ToString(), std::regex{"\x1b\\[[0-9;]*m"}, "");
}

bool has(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

// The screen's line holding `part`, or "".
std::string line_with(const std::string &text, const std::string &part) {
    const std::size_t at = text.find(part);
    if (at == std::string::npos) {
        return "";
    }
    const std::size_t start = text.rfind('\n', at);
    const std::size_t end = text.find('\n', at);
    return text.substr(start == std::string::npos ? 0 : start + 1,
                       end == std::string::npos ? std::string::npos : end - start - 1);
}

struct Fixture {
    std::vector<std::byte> bytes = day();
    DepthReplay replay{bytes, configs(bytes), kA};
    ReplayClock clock{kOpen, 60, 0};

    Fixture() {
        replay.advance_to(kOpen + 1'000);
    }
    [[nodiscard]] DepthFrame frame(DepthUi ui = {}) const {
        return make_frame(replay, clock, ui, "AAA");
    }
};

} // namespace

TEST(DepthRender, Helpers) {
    EXPECT_EQ(ordinal(1), "1st");
    EXPECT_EQ(ordinal(2), "2nd");
    EXPECT_EQ(ordinal(3), "3rd");
    EXPECT_EQ(ordinal(4), "4th");
    EXPECT_EQ(ordinal(11), "11th");
    EXPECT_EQ(ordinal(12), "12th");
    EXPECT_EQ(ordinal(22), "22nd");
    EXPECT_EQ(ordinal(113), "113th");
    EXPECT_EQ(speed_text(60), "60.0x");
    EXPECT_EQ(speed_text(0.25), "0.25x");
    const DepthRows rows = depth_rows(40, 10);
    EXPECT_EQ(rows.queue + rows.tape, 40U - 25U);
    EXPECT_GE(rows.queue, rows.tape);
    EXPECT_EQ(depth_rows(20, 10).queue, 0U); // too short: no room
}

TEST(DepthRender, HeaderAndLadders) {
    const Fixture fx;
    const std::string s = draw(fx.frame());
    EXPECT_TRUE(has(s, "NanoBook depth  AAA"));
    EXPECT_TRUE(has(s, "ITCH 09:30:00.000   60.0x   done")); // the day has no more
    EXPECT_TRUE(has(s, "bid 10.0000 x 570    ask 10.0200 x 70    spread 0.0200 (19.98 bps)"));
    EXPECT_TRUE(has(s, "6 resting    state T"));
    EXPECT_TRUE(has(s, "last trade 10.0000 x 30 at 09:30:00.000"));
    EXPECT_TRUE(has(s, "reference: "));
    EXPECT_TRUE(has(s, "matches ("));

    // The best bid and the best ask on one line, the bid's queue selected ('>').
    const std::string best = line_with(s, ">        3");
    EXPECT_TRUE(has(best, ">        3          570       10.0000"));
    EXPECT_TRUE(has(best, "10.0200                 70        1"));
    EXPECT_TRUE(has(line_with(s, "9.9900"), "50"));
    // The replaced order is at its new price; its old one is only on the tape.
    EXPECT_TRUE(has(line_with(s, "9.9900"), "10.0400                 80        1"));
    EXPECT_FALSE(has(line_with(s, "10.0300"), "|"));
}

TEST(DepthRender, QueueShowsSharesAhead) {
    const Fixture fx;
    const std::string s = draw(fx.frame());
    EXPECT_TRUE(has(s, "QUEUE  bid 10.0000  (3 orders, 570 shares; front first)"));
    EXPECT_TRUE(has(line_with(s, "     1               1"), "70             0"));
    const std::string second = line_with(s, "     2               2");
    EXPECT_TRUE(has(second, "200            70"));
    EXPECT_TRUE(has(second, "NSDQ"));
    EXPECT_TRUE(has(line_with(s, "     3               3"), "300           270"));
}

TEST(DepthRender, SelectingAnotherLevelAndSide) {
    const Fixture fx;
    std::string s = draw(fx.frame(DepthUi{.side = Side::Buy, .level = 5})); // clamped: 2nd level
    EXPECT_TRUE(has(s, "QUEUE  bid 9.9900  (1 order, 50 shares; front first)"));
    s = draw(fx.frame(DepthUi{.side = Side::Sell, .level = 1}));
    EXPECT_TRUE(has(s, "QUEUE  ask 10.0400"));
    EXPECT_TRUE(has(line_with(s, "10.0400"), "<"));
}

TEST(DepthRender, TapeNewestFirst) {
    const Fixture fx;
    const std::string s = draw(fx.frame(), 50); // tall enough for the whole tape
    const std::size_t replace = s.find("REPLACE  6 -> 7");
    const std::size_t cancel = s.find("CANCEL   99");
    const std::size_t exec = s.find("EXEC     1 ");
    ASSERT_NE(replace, std::string::npos);
    ASSERT_NE(cancel, std::string::npos);
    ASSERT_NE(exec, std::string::npos);
    EXPECT_LT(replace, cancel);
    EXPECT_LT(cancel, exec);
    EXPECT_TRUE(has(line_with(s, "REPLACE  6 -> 7"), "S          80 @ 10.0400"));
    EXPECT_TRUE(has(line_with(s, "CANCEL   99"), "?           1 @ ?  (UnknownOrder)"));
    EXPECT_TRUE(has(line_with(s, "ADD      2 "), "NSDQ"));
}

TEST(DepthRender, FollowedOrder) {
    Fixture fx;
    fx.replay.follow(OrderId{3});
    std::string s = draw(fx.frame());
    EXPECT_TRUE(has(s, "following 3: 3rd in line at bid 10.0000, 300 shares, 270 ahead of it"));
    fx.replay.follow(OrderId{6});
    s = draw(fx.frame());
    EXPECT_TRUE(has(s, "following 6: not in the book"));
}

TEST(DepthRender, ADifferenceIsShown) {
    const std::vector<std::byte> bytes = day();
    const std::vector<BookConfig> fast = configs(bytes);
    std::vector<BookConfig> reference = fast;
    reference[kA].max_orders = 1;
    DepthReplay replay{bytes, fast, reference, kA, ReplayOptions{}};
    replay.advance_to(kOpen + 1'000);
    const std::string s = draw(make_frame(replay, ReplayClock{kOpen, 1, 0}, DepthUi{}, "AAA"));
    EXPECT_TRUE(has(s, "stopped: the books differ"));
    EXPECT_TRUE(has(s, "DIFFERS at 09:30:00.000"));
}

TEST(DepthRender, AFrameFits110Columns) {
    const Fixture fx;
    const std::string s = draw(fx.frame());
    EXPECT_TRUE(has(s, "q quit"));
    EXPECT_TRUE(has(s, "(UnknownOrder)")); // the tape's longest line, uncut
    EXPECT_TRUE(has(s, "ATTRIBUTION"));
}

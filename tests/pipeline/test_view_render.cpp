/*
 * test_view_render.cpp: tests for the viewer's screen layout (tools/view_render.hpp).
 *
 * What it does: Renders frames into an off-screen FTXUI Screen and reads them back as text: the
 *   column names line up with a stock's line; a frame shows the table's name, the state, the
 *   ITCH clock, the latency line (or why there is none) and each stock's prices; only as many
 *   stocks as fit are drawn, and the list scrolls so the selected one stays visible; a waiting
 *   viewer says what it is waiting for.
 *
 * Why it exists: The layout is the part of the viewer a person reads. These tests catch a column
 *   that slipped or a line that went missing without anyone having to look.
 *
 * How it works: ftxui::Screen(width, height) plus ftxui::Render() draw without a terminal;
 *   Screen::ToString() gives the text. Built only with -DNANOBOOK_BUILD_VIEWER=ON (its own
 *   program, nanobook_view_tests, because it links FTXUI).
 */

#include <gtest/gtest.h>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "view_render.hpp" // tools/

using namespace nanobook::view;

namespace {

struct Prices {
    std::int64_t bid;
    std::int64_t ask;
};

Row stock(const std::string &name, std::uint16_t locate, Prices p) {
    Row r{.locate = locate};
    r.symbol.fill(' ');
    std::copy_n(name.begin(), std::min(name.size(), r.symbol.size()), r.symbol.begin());
    r.quote = Quote{.sequence = 1,
                    .timestamp = 0,
                    .bid_price = p.bid,
                    .bid_qty = 300,
                    .ask_price = p.ask,
                    .ask_qty = 1'200,
                    .updates = 1'234'567};
    r.updates_per_second = 4'321;
    return r;
}

std::string draw(const Health &h, const std::vector<Row> &rows, const UiState &ui, int width,
                 int height) {
    auto screen = ftxui::Screen(width, height);
    ftxui::Render(screen, render(h, rows, ui, visible_rows(height)));
    return screen.ToString();
}

UiState attached_to(const std::string &name) {
    UiState ui;
    ui.name = name;
    ui.attached = true;
    return ui;
}

std::size_t count(const std::string &text, const std::string &what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

} // namespace

TEST(ViewRender, ColumnNamesLineUpWithAStocksLine) {
    const std::string names = column_names();
    const std::string line = row_text(stock("AAPL", 1, {.bid = 1'650'500, .ask = 1'650'600}));
    EXPECT_EQ(names.size(), line.size());
    // Each column's right edge: the names' last letter and the value's last digit share a place.
    EXPECT_EQ(names.find("BID SIZE") + 8, line.find("300") + 3);
    const std::size_t bid = names.find(" BID ", names.find("BID SIZE") + 1); // not BID SIZE
    EXPECT_EQ(bid + 4, line.find("165.0500") + 8);
    EXPECT_EQ(names.find("ASK SIZE") + 8, line.find("1,200") + 5);
    EXPECT_EQ(names.find("UPDATES") + 7, line.find("1,234,567") + 9);
    EXPECT_NE(line.find("0.0100"), std::string::npos); // the spread, $0.01
    EXPECT_NE(line.find("0.61"), std::string::npos);   // in basis points
}

TEST(ViewRender, AFrameShowsTheRunAndEveryStock) {
    Health h{.state = RunState::Running,
             .updates = 12'186'642,
             .itch_time = 34'264'123'456'789, // 09:31:04.123
             .updates_per_second = 1'297'278,
             .speed = 60.0,
             .alive = true,
             .paced = true,
             .latency =
                 LatencyWindow{.count = 1'297'278, .p50_ns = 410, .p99_ns = 650, .p999_ns = 1'500}};
    const std::vector<Row> rows{stock("AAPL", 1, {.bid = 1'650'500, .ask = 1'650'600}),
                                stock("MSFT", 2, {.bid = 4'000'000, .ask = 0})};
    const UiState ui = attached_to("nanobook");
    const std::string frame = draw(h, rows, ui, 108, 20); // the README's width: nothing cut off
    EXPECT_NE(frame.find("/dev/shm/nanobook"), std::string::npos) << frame;
    EXPECT_NE(frame.find("running"), std::string::npos);
    EXPECT_NE(frame.find("ITCH 09:31:04.123"), std::string::npos);
    EXPECT_NE(frame.find("speed 60.0x"), std::string::npos);
    EXPECT_NE(frame.find("1,297,278 updates/s"), std::string::npos);
    EXPECT_NE(frame.find("p99 0.65 us"), std::string::npos);
    EXPECT_NE(frame.find("sped-up bursts queue"), std::string::npos); // 60x: the tail is queueing
    EXPECT_NE(frame.find("165.0500"), std::string::npos);
    EXPECT_NE(frame.find("400.0000"), std::string::npos);
    EXPECT_NE(frame.find("sort: activity"), std::string::npos);

    h.paced = false;
    h.latency.reset();
    EXPECT_NE(draw(h, rows, ui, 120, 20).find("unpaced run"), std::string::npos);
}

TEST(ViewRender, OnlyWhatFitsIsDrawnAndTheCursorStaysOnScreen) {
    const std::vector<std::string> names{"AA", "BB", "CC", "DD", "EE", "FF", "GG", "HH"};
    std::vector<Row> rows;
    rows.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        rows.push_back(
            stock(names[i], static_cast<std::uint16_t>(i + 1), {.bid = 100, .ask = 200}));
    }
    Health h;
    h.state = RunState::Running;
    UiState ui = attached_to("nanobook");
    const int height = 9 + 3; // room for three stocks
    ASSERT_EQ(visible_rows(height), 3U);
    std::string frame = draw(h, rows, ui, 120, height);
    EXPECT_EQ(count(frame, "  AA "), 1U) << frame;
    EXPECT_EQ(count(frame, "  CC "), 1U);
    EXPECT_EQ(count(frame, "  DD "), 0U); // doesn't fit
    ui.cursor = 6;                        // GG: the list scrolls to show EE, FF, GG
    frame = draw(h, rows, ui, 120, height);
    EXPECT_EQ(count(frame, "  AA "), 0U);
    EXPECT_EQ(count(frame, "  EE "), 1U);
    EXPECT_EQ(count(frame, "  GG "), 1U);
    EXPECT_EQ(count(frame, "  HH "), 0U);
}

TEST(ViewRender, AWaitingViewerSaysWhatItIsWaitingFor) {
    UiState ui;
    ui.name = "nanobook";
    ui.waiting = "no table yet";
    const std::string frame = draw(Health{}, {}, ui, 120, 12);
    EXPECT_NE(frame.find("waiting: no table yet"), std::string::npos) << frame;
    EXPECT_EQ(frame.find("ITCH"), std::string::npos); // no numbers until there is a table
}

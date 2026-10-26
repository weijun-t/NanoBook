/*
 * view_render.hpp: lays the viewer's screen out with FTXUI: the health panel, the watchlist, keys.
 *
 * What it does: render() turns the Watchlist's numbers (support/watchlist.hpp) and what the user
 *   has typed (UiState) into one FTXUI Element: a title line with the table's name and the run's
 *   state; the health lines (ITCH clock, speed, updates per second, latency of the last second);
 *   one line per stock (symbol, bid size, bid, ask, ask size, spread, bps, updates per second,
 *   updates so far), as many as fit; and the keys.
 *
 * Why it exists: Viewer design §6. Drawing only: every number arrives already worked out, so the
 *   tests can render a frame into an off-screen Screen and read it back as text
 *   (test_view_render.cpp) without a terminal.
 *
 * How it works: Each stock line is one text() of fixed-width columns (snprintf, one format shared
 *   with the column names), so a frame is a few dozen elements, not one per cell. The selected line
 * is inverted; a pinned stock has a '*'; a crossed book is red. When the cursor goes below the last
 * visible line, the list scrolls so the cursor stays on screen. O(visible lines) per frame.
 *
 * Trade-offs: Viewer-side code: strings are built and thrown away every frame (10 a second), which
 *   costs nothing that matters here.
 */

#pragma once

#include <ftxui/dom/elements.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>

#include "watchlist.hpp" // support/

namespace nanobook::view {

struct UiState {
    std::string name;     // the table's name, /dev/shm/<name>
    bool attached{false}; // a table is open
    std::string waiting;  // why not, if not
    std::string filter;   // symbol prefix
    bool typing{false};   // '/' pressed: keys go to the filter
    SortBy sort{SortBy::Activity};
    std::size_t cursor{0}; // the selected line, in the filtered, sorted list
};

inline const char *sort_name(SortBy sort) {
    switch (sort) {
    case SortBy::Activity:
        return "activity";
    case SortBy::Symbol:
        return "symbol";
    case SortBy::Spread:
        return "spread";
    }
    return "?";
}

// 1234567 -> "1,234,567".
inline std::string with_commas(std::uint64_t n) {
    std::string digits = std::to_string(n);
    for (std::size_t i = digits.size(); i > 3; i -= 3) {
        digits.insert(i - 3, ",");
    }
    return digits;
}

// One format for the column names and every stock line, so the two always line up.
inline std::string columns(char mark, const std::string &symbol, const std::string &bid_qty,
                           const std::string &bid, const std::string &ask,
                           const std::string &ask_qty, const std::string &spread,
                           const std::string &bps, const std::string &rate,
                           const std::string &updates) {
    std::array<char, 160> line{};
    std::snprintf(line.data(), line.size(), "%c %-8s %11s %12s %12s %11s %9s %8s %9s %12s", mark,
                  symbol.c_str(), bid_qty.c_str(), bid.c_str(), ask.c_str(), ask_qty.c_str(),
                  spread.c_str(), bps.c_str(), rate.c_str(), updates.c_str());
    return line.data();
}

inline std::string column_names() {
    return columns(' ', "SYMBOL", "BID SIZE", "BID", "ASK", "ASK SIZE", "SPREAD", "BPS", "UPD/S",
                   "UPDATES");
}

inline std::string row_text(const Row &r) {
    const Quote &q = r.quote;
    const auto qty = [](std::int64_t n, std::int64_t price) {
        return price > 0 ? with_commas(static_cast<std::uint64_t>(n)) : std::string{"-"};
    };
    const auto bps = spread_bps(q);
    std::array<char, 16> bps_text{'-'};
    if (bps) {
        std::snprintf(bps_text.data(), bps_text.size(), "%.2f", *bps);
    }
    return columns(
        r.pinned ? '*' : ' ', std::string{trimmed(r.symbol)}, qty(q.bid_qty, q.bid_price),
        format_price(q.bid_price), format_price(q.ask_price), qty(q.ask_qty, q.ask_price),
        two_sided(q) ? format_price(q.ask_price - q.bid_price) : "-", bps_text.data(),
        with_commas(static_cast<std::uint64_t>(r.updates_per_second)), with_commas(q.updates));
}

inline std::string state_text(const Health &h, const UiState &ui) {
    if (!ui.attached) {
        return "waiting: " + ui.waiting;
    }
    switch (h.state) {
    case RunState::Starting:
        return "starting (the pipeline is sizing the books)";
    case RunState::Running:
        return h.alive ? "running" : "running, no updates in the last second";
    case RunState::Done:
        return "done";
    }
    return "?";
}

inline std::string health_text(const Health &h) {
    std::array<char, 64> rate{};
    std::snprintf(rate.data(), rate.size(), "%.1fx", h.speed);
    return "ITCH " + format_time(h.itch_time) + "   speed " + (h.paced ? rate.data() : "unpaced") +
           "   " + with_commas(static_cast<std::uint64_t>(h.updates_per_second)) + " updates/s   " +
           with_commas(h.updates) + " in all";
}

inline std::string latency_text(const Health &h) {
    if (!h.paced) {
        return "latency: unpaced run (start nanobook_pipeline with --speed N to measure it)";
    }
    if (!h.latency) {
        return "latency: no paced updates in the last second";
    }
    std::array<char, 200> text{};
    std::snprintf(text.data(), text.size(),
                  "latency, last second (%s updates): p50 %.2f us   p99 %.2f us   p99.9 %.2f us",
                  with_commas(h.latency->count).c_str(), h.latency->p50_ns / 1e3,
                  h.latency->p99_ns / 1e3, h.latency->p999_ns / 1e3);
    // Above 1x a burst arrives `speed` times denser than it did, so the tail is mostly queueing
    // behind compressed bursts (measured: p99 ~40 us at 1x, 0.6-2.4 ms at 60x, WSL2).
    return std::string{text.data()} + (h.speed > 1.5 ? "   (sped-up bursts queue)" : "");
}

// The first visible line, so that `cursor` is on screen.
inline std::size_t first_visible(std::size_t cursor, std::size_t visible) {
    return visible == 0 || cursor < visible ? 0 : cursor - visible + 1;
}

inline ftxui::Element render(const Health &h, std::span<const Row> rows, const UiState &ui,
                             std::size_t visible) {
    ftxui::Elements lines;
    const std::size_t first = first_visible(ui.cursor, visible);
    for (std::size_t i = first; i < rows.size() && i < first + visible; ++i) {
        ftxui::Element line = ftxui::text(row_text(rows[i]));
        if (crossed(rows[i].quote)) {
            line = line | ftxui::color(ftxui::Color::Red);
        }
        if (i == ui.cursor) {
            line = line | ftxui::inverted;
        }
        lines.push_back(std::move(line));
    }
    if (rows.empty() && ui.attached) {
        lines.push_back(ftxui::text(ui.filter.empty()
                                        ? "  (no stocks yet)"
                                        : "  (no symbol starts with " + ui.filter + ")") |
                        ftxui::dim);
    }
    const std::string filter = ui.typing ? "filter: " + ui.filter + "_"
                                         : (ui.filter.empty() ? "" : "filter: " + ui.filter);
    return ftxui::vbox({
               ftxui::hbox({ftxui::text(" NanoBook ") | ftxui::bold,
                            ftxui::text("/dev/shm/" + ui.name), ftxui::filler(),
                            ftxui::text(state_text(h, ui) + " ")}),
               ftxui::text(ui.attached ? " " + health_text(h) : ""),
               ftxui::text(ui.attached ? " " + latency_text(h) : ""),
               ftxui::separator(),
               ftxui::text(column_names()) | ftxui::bold,
               ftxui::vbox(std::move(lines)),
               ftxui::filler(),
               ftxui::separator(),
               ftxui::hbox({ftxui::text(" up/down move  p pin  / filter  s sort: " +
                                        std::string{sort_name(ui.sort)} + "  q quit"),
                            ftxui::filler(), ftxui::text(filter + " ")}),
           }) |
           ftxui::border;
}

// Lines a terminal of `height` rows has for stocks: all but the border (2), the title and
// health lines (3), two separators, the column names and the keys.
inline std::size_t visible_rows(int height) {
    constexpr int kOther = 9;
    return height > kOther ? static_cast<std::size_t>(height - kOther) : 0;
}

} // namespace nanobook::view

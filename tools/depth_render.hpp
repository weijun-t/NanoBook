/*
 * depth_render.hpp: lays nanobook_depth's screen out with FTXUI: one stock's ladder, queue, tape.
 *
 * What it does: make_frame() copies what the screen needs out of a DepthReplay
 *   (support/depth_replay.hpp) and the ReplayClock: the header numbers, the best levels of each
 *   side (L2), the queue at the selected level (L3), the tape, the followed order and the live
 *   check. render() turns a DepthFrame into one FTXUI Element: a header (stock, ITCH clock,
 *   speed, state; best bid and ask; last trade and the check; the followed order), the two
 *   ladders side by side, the queue, the tape and the keys.
 *
 * Why it exists: Depth design §4. Drawing only: the tests fill a DepthFrame (or make one from a
 *   small replay), render it into an off-screen Screen and read it back as text
 *   (test_depth_render.cpp).
 *
 * How it works: Each line is one text() of fixed-width columns (snprintf), so a frame is a few
 *   dozen elements. The selected level is inverted; the followed order's level and its line in
 *   the queue are yellow; a difference between the books turns the check red. The queue and the
 *   tape share whatever height is left after the fixed lines (depth_rows()).
 *
 * Trade-offs: Viewer-side code: strings are built and thrown away every frame (10 a second).
 */

#pragma once

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "depth_replay.hpp" // support/
#include "nanobook/core/names.hpp"
#include "view_render.hpp" // with_commas()
#include "watchlist.hpp"   // format_price(), format_time(), trimmed()

namespace nanobook::depth {

using view::format_price;
using view::format_time;
using view::with_commas;

// What the user has chosen: the selected level and side.
struct DepthUi {
    Side side{Side::Buy};
    std::size_t level{0}; // 0 = the best
    std::size_t levels{10};
};

struct DepthFrame {
    std::string symbol;
    std::uint64_t itch_time{};
    double speed{1};
    std::string state; // running, paused, done, stopped: ...
    char trading_state{'?'};
    std::vector<LevelRow> bids;
    std::vector<LevelRow> asks;
    std::size_t resting{};
    std::optional<Trade> last_trade;
    std::uint64_t stock_messages{};
    bool check_ok{true};
    std::string check; // "matches (N checks)" or what differs
    Side side{Side::Buy};
    std::size_t level{}; // selected, within its side's rows
    std::optional<Price> queue_price;
    std::vector<QueueRow> queue;
    std::vector<TapeEntry> tape;
    std::optional<FollowStatus> follow;
};

// 1 -> "1st", 2 -> "2nd", 13 -> "13th".
inline std::string ordinal(std::size_t n) {
    const std::size_t last_two = n % 100;
    const char *suffix = "th";
    if (last_two < 11 || last_two > 13) {
        switch (n % 10) {
        case 1:
            suffix = "st";
            break;
        case 2:
            suffix = "nd";
            break;
        case 3:
            suffix = "rd";
            break;
        default:
            break;
        }
    }
    return std::to_string(n) + suffix;
}

inline std::string speed_text(double speed) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), speed >= 1 ? "%.1fx" : "%.3gx", speed);
    return text.data();
}

// The side as the screen says it: the bid or the ask.
inline const char *side_word(Side side) {
    return side == Side::Buy ? "bid" : "ask";
}

inline std::string shares_text(Qty q) {
    return with_commas(static_cast<std::uint64_t>(std::max<std::int64_t>(q.value, 0)));
}

inline std::string state_text(const DepthReplay &replay, const ReplayClock &clock) {
    if (replay.mismatch()) {
        return "stopped: the books differ";
    }
    if (replay.error()) {
        return "stopped: broken message at byte " + with_commas(replay.error()->offset);
    }
    if (replay.at_end()) {
        return "done";
    }
    return clock.paused() ? "paused" : "running";
}

inline DepthFrame make_frame(const DepthReplay &replay, const ReplayClock &clock, const DepthUi &ui,
                             const std::string &symbol) {
    DepthFrame f;
    f.symbol = symbol;
    f.itch_time = replay.itch_time();
    f.speed = clock.speed();
    f.state = state_text(replay, clock);
    f.trading_state = replay.trading_state();
    f.bids = replay.levels(Side::Buy, ui.levels);
    f.asks = replay.levels(Side::Sell, ui.levels);
    f.resting = replay.book().order_count();
    f.last_trade = replay.last_trade();
    f.stock_messages = replay.stock_messages();
    f.side = ui.side;
    f.follow = replay.follow_status();
    if (const auto &m = replay.mismatch()) {
        f.check_ok = false;
        f.check = "DIFFERS at " + format_time(m->timestamp) + ": " + m->what;
    } else {
        f.check = "matches (" + with_commas(replay.quick_checks()) + " checks)";
    }
    const std::vector<LevelRow> &rows = ui.side == Side::Buy ? f.bids : f.asks;
    if (!rows.empty()) {
        f.level = std::min(ui.level, rows.size() - 1);
        f.queue_price = rows[f.level].price;
        f.queue = replay.queue(ui.side, *f.queue_price);
    }
    f.tape.assign(replay.tape().begin(), replay.tape().end());
    return f;
}

// ---- lines ------------------------------------------------------------------

inline std::string quote_text(const DepthFrame &f) {
    const auto side = [](const std::vector<LevelRow> &rows) {
        return rows.empty()
                   ? std::string{"-"}
                   : format_price(rows[0].price.ticks) + " x " + shares_text(rows[0].shares);
    };
    std::string text = " bid " + side(f.bids) + "    ask " + side(f.asks);
    if (!f.bids.empty() && !f.asks.empty()) {
        const std::int64_t bid = f.bids[0].price.ticks;
        const std::int64_t ask = f.asks[0].price.ticks;
        std::array<char, 32> bps{};
        std::snprintf(bps.data(), bps.size(), "%.2f",
                      static_cast<double>(ask - bid) * 20'000.0 / static_cast<double>(ask + bid));
        text += "    spread " + (ask > bid ? format_price(ask - bid) : std::string{"crossed"}) +
                " (" + bps.data() + " bps)";
    }
    return text + "    " + with_commas(f.resting) + " resting    state " + f.trading_state;
}

inline std::string trade_text(const DepthFrame &f) {
    std::string text = f.last_trade ? " last trade " + format_price(f.last_trade->price.ticks) +
                                          " x " + shares_text(f.last_trade->shares) + " at " +
                                          format_time(f.last_trade->timestamp)
                                    : std::string{" no trade yet"};
    return text + "    messages " + with_commas(f.stock_messages) + "    reference: ";
}

inline std::string follow_text(const DepthFrame &f) {
    if (!f.follow) {
        return "";
    }
    const FollowStatus &s = *f.follow;
    std::string text = " following " + with_commas(s.id.value) + ": ";
    switch (s.state) {
    case FollowState::Resting:
        return text + ordinal(s.place) + " in line at " + side_word(s.side) + " " +
               format_price(s.price.ticks) + ", " + shares_text(s.shares) + " shares, " +
               shares_text(s.ahead) + " ahead of it";
    case FollowState::Filled:
        return text + "filled";
    case FollowState::Cancelled:
        return text + "cancelled";
    case FollowState::NotResting:
        return text + "not in the book";
    }
    return text;
}

// One side's ladder line; bids read ORDERS SHARES PRICE, asks PRICE SHARES ORDERS, so the two
// best prices meet in the middle.
inline std::string bid_line(char mark, const std::string &orders, const std::string &shares,
                            const std::string &price) {
    std::array<char, 96> line{};
    std::snprintf(line.data(), line.size(), "%c %8s %12s %13s ", mark, orders.c_str(),
                  shares.c_str(), price.c_str());
    return line.data();
}
inline std::string ask_line(char mark, const std::string &orders, const std::string &shares,
                            const std::string &price) {
    std::array<char, 96> line{};
    std::snprintf(line.data(), line.size(), " %-13s %12s %8s %c", price.c_str(), shares.c_str(),
                  orders.c_str(), mark);
    return line.data();
}

inline std::string queue_line(const std::string &place, const std::string &id,
                              const std::string &shares, const std::string &ahead,
                              const std::string &since, const std::string &attribution) {
    std::array<char, 128> line{};
    std::snprintf(line.data(), line.size(), " %6s  %14s  %10s  %12s  %-12s  %s", place.c_str(),
                  id.c_str(), shares.c_str(), ahead.c_str(), since.c_str(), attribution.c_str());
    return line.data();
}

inline std::string queue_row_text(std::size_t place, const QueueRow &row) {
    const bool named = row.attribution != Mpid{};
    return queue_line(std::to_string(place), with_commas(row.id.value), shares_text(row.shares),
                      shares_text(row.ahead), row.since != 0 ? format_time(row.since) : "?",
                      named ? std::string{row.attribution.data(), row.attribution.size()} : "");
}

inline const char *tape_kind(TapeKind kind) {
    switch (kind) {
    case TapeKind::Add:
        return "ADD";
    case TapeKind::Execute:
        return "EXEC";
    case TapeKind::Cancel:
        return "CANCEL";
    case TapeKind::Delete:
        return "DELETE";
    case TapeKind::Replace:
        return "REPLACE";
    }
    return "?";
}

inline std::string tape_text(const TapeEntry &e) {
    std::string ids = with_commas(e.id.value);
    if (e.kind == TapeKind::Replace) {
        ids += " -> " + with_commas(e.new_id.value);
    }
    std::array<char, 160> line{};
    std::snprintf(line.data(), line.size(), " %-12s  %-7s  %-28s  %c  %10s @ %s",
                  format_time(e.timestamp).c_str(), tape_kind(e.kind), ids.c_str(),
                  e.price.ticks <= 0 ? '?' : (e.side == Side::Buy ? 'B' : 'S'),
                  shares_text(e.shares).c_str(),
                  e.price.ticks > 0 ? format_price(e.price.ticks).c_str() : "?");
    std::string text = line.data();
    if (e.attribution != Mpid{}) {
        text += "  " + std::string{e.attribution.data(), e.attribution.size()};
    }
    if (!e.result) {
        text += "  (not applied: bad input)";
    } else if (*e.result != FeedResult::Ok) {
        text += "  (" + std::string{name(*e.result)} + ")";
    }
    return text;
}

// ---- the frame --------------------------------------------------------------

// How many queue and tape lines a terminal of `height` rows has room for. Fixed: the border
// (2), 4 header lines, 4 separators, the ladder's names, `levels` ladder lines, the queue's title
// and names (2), the tape's title, the keys.
struct DepthRows {
    std::size_t queue{};
    std::size_t tape{};
};
inline DepthRows depth_rows(int height, std::size_t levels) {
    const auto fixed = static_cast<int>(15 + levels);
    if (height <= fixed) {
        return {};
    }
    const auto left = static_cast<std::size_t>(height - fixed);
    return DepthRows{.queue = (left + 1) / 2, .tape = left / 2};
}

inline ftxui::Element render(const DepthFrame &f, std::size_t levels, DepthRows rows) {
    using namespace ftxui;
    const std::optional<Price> followed_price = f.follow && f.follow->state == FollowState::Resting
                                                    ? std::optional{f.follow->price}
                                                    : std::nullopt;
    const auto ladder = [&](Side side, const std::vector<LevelRow> &level_rows) {
        Elements lines;
        lines.push_back(text(side == Side::Buy ? bid_line(' ', "ORDERS", "SHARES", "BID")
                                               : ask_line(' ', "ORDERS", "SHARES", "ASK")) |
                        bold);
        for (std::size_t i = 0; i < levels; ++i) {
            if (i >= level_rows.size()) {
                lines.push_back(text(""));
                continue;
            }
            const LevelRow &r = level_rows[i];
            const bool selected = side == f.side && i == f.level;
            const char mark = selected ? '>' : ' ';
            const std::string orders = with_commas(r.orders);
            Element line = text(
                side == Side::Buy
                    ? bid_line(mark, orders, shares_text(r.shares), format_price(r.price.ticks))
                    : ask_line(selected ? '<' : ' ', orders, shares_text(r.shares),
                               format_price(r.price.ticks)));
            if (followed_price && side == f.follow->side && r.price == *followed_price) {
                line = line | color(Color::Yellow);
            }
            if (selected) {
                line = line | inverted;
            }
            lines.push_back(std::move(line));
        }
        return vbox(std::move(lines));
    };

    Elements queue;
    for (std::size_t i = 0; i < f.queue.size() && i < rows.queue; ++i) {
        const bool more = i + 1 == rows.queue && f.queue.size() > rows.queue;
        if (more) {
            queue.push_back(text("   ... and " + with_commas(f.queue.size() - i) + " more") | dim);
            break;
        }
        Element line = text(queue_row_text(i + 1, f.queue[i]));
        if (f.follow && f.queue[i].id == f.follow->id) {
            line = line | color(Color::Yellow);
        }
        queue.push_back(std::move(line));
    }
    std::string queue_title = " QUEUE  (no orders on this side)";
    if (f.queue_price) {
        Qty total{0};
        for (const QueueRow &r : f.queue) {
            total += r.shares;
        }
        queue_title = " QUEUE  " + std::string{side_word(f.side)} + " " +
                      format_price(f.queue_price->ticks) + "  (" + with_commas(f.queue.size()) +
                      (f.queue.size() == 1 ? " order, " : " orders, ") + shares_text(total) +
                      " shares; front first)";
    }

    Elements tape;
    for (std::size_t i = 0; i < f.tape.size() && i < rows.tape; ++i) {
        tape.push_back(text(tape_text(f.tape[i])));
    }

    Element check = text(f.check);
    check = f.check_ok ? check | color(Color::Green) : check | color(Color::Red) | bold;
    return vbox({
               hbox({text(" NanoBook depth  ") | bold, text(f.symbol) | bold, filler(),
                     text("ITCH " + format_time(f.itch_time) + "   " + speed_text(f.speed) + "   " +
                          f.state + " ")}),
               text(quote_text(f)),
               hbox({text(trade_text(f)), check}),
               text(follow_text(f)) | color(Color::Yellow),
               separator(),
               hbox({ladder(Side::Buy, f.bids) | flex, separator(),
                     ladder(Side::Sell, f.asks) | flex}),
               separator(),
               text(queue_title) | bold,
               text(queue_line("PLACE", "ORDER ID", "SHARES", "AHEAD", "SINCE", "ATTRIBUTION")) |
                   dim,
               vbox(std::move(queue)),
               filler(),
               separator(),
               text(" TAPE  " + f.symbol + ", newest first") | bold,
               vbox(std::move(tape)),
               separator(),
               text(" up/down level  tab side  space pause  n step  f follow  +/- speed  q quit"),
           }) |
           border;
}

} // namespace nanobook::depth

/*
 * nanobook_depth.cpp: one stock's whole book, live: price levels (L2), queues (L3) and its tape.
 *
 * What it does: Replays the day's ITCH file for one stock (support/depth_replay.hpp) with the
 *   engine's own FastBook and phase 1's ReferenceBook as a live check, paced by the ITCH clock,
 *   and shows it full screen ten times a second (tools/depth_render.hpp): the best levels of each
 *   side, the queue at the selected level (each order's shares, the shares ahead of it, when it
 *   arrived), the stock's latest messages, and an order you follow until it fills or leaves.
 *     nanobook_depth FILE SYMBOL [--speed N] [--from HH:MM:SS] [--levels N]
 *   Defaults: --speed 1, --from 09:30:00, --levels 10. Keys: up/down choose a level, Tab
 *   switches side, Space pauses and resumes, n applies the stock's next message (pausing first),
 *   f follows the newest order in the shown queue (again: stop following), + and - double and
 *   halve the speed, q or Esc quits.
 *     nanobook_depth FILE --check-day SYMBOL[,SYMBOL...]
 *   No screen: replays the whole day for each stock, with the live check, next to a full
 *   replay of every stock, and compares the two copies of each stock's book in full at the open,
 *   every hour to 16:00 and the end. PASS or FAIL per stock; exit 0 if all pass.
 *
 * Why it exists: Depth design, option A: a second firm's copy of one book, in its own process.
 *   The engine and the pipeline never know it exists, and no published number changes.
 *
 * How it works: Start-up runs the counting pass (as the pipeline does) to size the stock's book,
 *   then replays unpaced up to --from. After that a ticker thread posts an FTXUI event every
 *   100 ms; the UI thread advances the replay to the clock's ITCH time and redraws. One thread
 *   touches the replay, so nothing is locked (depth design §3.3). If the books ever differ, the
 *   replay stops right after that message and the screen says so in red.
 *
 * Trade-offs: A tool: it allocates and prints freely. Every stock's messages are parsed to find
 *   one stock's, so start-up takes about as long as the pipeline's. Built only with
 *   -DNANOBOOK_BUILD_VIEWER=ON.
 */

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "depth_render.hpp"
#include "depth_replay.hpp"           // support/
#include "itch_replay_comparison.hpp" // support/
#include "itch_timing.hpp"            // support/
#include "mapped_file.hpp"            // support/
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/sizing.hpp"

namespace {

using namespace nanobook;
using namespace nanobook::depth;

constexpr std::size_t kWindow = std::size_t{1} << 30U; // counting pass: 1 GiB at a time
constexpr std::uint64_t kNsPerSecond = 1'000'000'000;

const char *const kUsage =
    "usage: nanobook_depth FILE SYMBOL [--speed N] [--from HH:MM:SS] [--levels N]\n"
    "       nanobook_depth FILE --check-day SYMBOL[,SYMBOL...]\n";

struct Options {
    std::string file;
    std::string symbol;
    std::vector<std::string> check_day;
    double speed{1};
    std::uint64_t from{(9 * 3600 + 30 * 60) * kNsPerSecond}; // 09:30:00
    std::size_t levels{10};
};

template <typename T> bool parse_number(std::string_view text, T &value) {
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

// "HH:MM:SS" -> nanoseconds since midnight.
bool parse_time(std::string_view text, std::uint64_t &ns) {
    std::uint64_t h = 0;
    std::uint64_t m = 0;
    std::uint64_t s = 0;
    if (text.size() != 8 || text[2] != ':' || text[5] != ':' ||
        !parse_number(text.substr(0, 2), h) || !parse_number(text.substr(3, 2), m) ||
        !parse_number(text.substr(6, 2), s) || h > 23 || m > 59 || s > 59) {
        return false;
    }
    ns = ((h * 60 + m) * 60 + s) * kNsPerSecond;
    return true;
}

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        parts.emplace_back(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return parts;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options o;
    std::vector<std::string_view> plain;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (!arg.starts_with("--")) {
            plain.push_back(arg);
            continue;
        }
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        bool ok = true;
        if (arg == "--speed") {
            ok = parse_number(value, o.speed) && o.speed >= ReplayClock::kMinSpeed &&
                 o.speed <= ReplayClock::kMaxSpeed;
        } else if (arg == "--from") {
            ok = parse_time(value, o.from);
        } else if (arg == "--levels") {
            ok = parse_number(value, o.levels) && o.levels >= 1 && o.levels <= 50;
        } else if (arg == "--check-day") {
            o.check_day = split(value);
            ok = !o.check_day.empty();
        } else {
            ok = false;
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    const std::size_t wanted = o.check_day.empty() ? 2 : 1;
    if (plain.size() != wanted) {
        return std::nullopt;
    }
    o.file = plain[0];
    if (wanted == 2) {
        o.symbol = plain[1];
    }
    return o;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

double seconds_since(std::uint64_t start) {
    return static_cast<double>(now_ns() - start) / 1e9;
}

// The counting pass: the day's directory and every stock's counts, for sizing (sizing.hpp).
std::optional<itch::DayCounter> count_day(std::span<const std::byte> bytes) {
    std::cerr << "counting the day's orders, to size the book...\n";
    const std::uint64_t start = now_ns();
    itch::DayCounter counter{1 << 22};
    const auto counted = itch::replay_in_windows(bytes, kWindow, counter);
    if (!counted || counted->bytes != bytes.size()) {
        std::cerr << "the file is broken or ends in the middle of a message\n";
        return std::nullopt;
    }
    std::cerr << "  done in " << seconds_since(start) << " s\n";
    return counter;
}

std::optional<std::uint16_t> locate_of(const itch::DayCounter &counter, std::string_view text) {
    const std::optional<Symbol> symbol = to_symbol(text);
    const std::optional<std::uint16_t> locate =
        symbol ? find_locate(counter.directory(), *symbol) : std::nullopt;
    if (!locate) {
        std::cerr << "no stock called " << text << " in this file\n";
    }
    return locate;
}

// The moments --check-day compares at: the open, every hour to the close, and the day's end
// (when Nasdaq has cleared every book, so only the earlier ones compare real books).
std::vector<std::uint64_t> checkpoints() {
    std::vector<std::uint64_t> times{(9 * 3600 + 30 * 60) * kNsPerSecond};
    for (std::uint64_t hour = 10; hour <= 16; ++hour) {
        times.push_back(hour * 3600 * kNsPerSecond);
    }
    times.push_back(std::numeric_limits<std::uint64_t>::max());
    return times;
}

std::string checkpoint_name(std::uint64_t t) {
    return t == std::numeric_limits<std::uint64_t>::max() ? std::string{"the end"}
                                                          : format_time(t).substr(0, 5);
}

// One stock's replay, and what comparing it with the full replay found.
struct Checked {
    std::string symbol;
    std::uint16_t locate{};
    std::unique_ptr<DepthReplay> replay;
    std::size_t equal{};       // checkpoints where the two books were equal
    std::size_t most_orders{}; // the most resting orders at a checkpoint...
    std::uint64_t most_at{};   // ...and when
    std::optional<std::string> first_difference;
};

// --check-day: each stock's replay against a full replay of every stock, compared in full at
// every checkpoint, with the reference book checking it after every message all day.
int check_day(std::span<const std::byte> bytes, const Options &o) {
    const std::optional<itch::DayCounter> counter = count_day(bytes);
    if (!counter) {
        return 1;
    }
    std::vector<Checked> stocks;
    for (const std::string &s : o.check_day) {
        const auto locate = locate_of(*counter, s);
        if (!locate) {
            return 1;
        }
        Checked c;
        c.symbol = s;
        c.locate = *locate;
        c.replay = std::make_unique<DepthReplay>(
            bytes, one_stock_configs(counter->directory(), counter->stats(), *locate), *locate,
            ReplayOptions{.full_every = 1024, .release_behind = true});
        stocks.push_back(std::move(c));
    }

    std::cerr << "replaying every stock and " << stocks.size()
              << " one-stock replays side by side...\n";
    const std::uint64_t start = now_ns();
    itch::BookBuilder<FastBook> full{
        itch::size_books(counter->directory(), counter->stats(), itch::SizingRules{}),
        itch::BuilderOptions{}};
    MessageCursor cursor{bytes, true};
    const std::vector<std::uint64_t> times = checkpoints();
    for (const std::uint64_t t : times) {
        while (!cursor.done() && cursor.next_timestamp() <= t) {
            if (const auto parsed = cursor.next(full); !parsed) {
                std::cerr << "the full replay failed at byte " << parsed.error().offset << "\n";
                return 1;
            }
        }
        for (Checked &c : stocks) {
            c.replay->advance_to(t);
            const auto diff = itch::compare_books(c.replay->book(), full.book(c.locate));
            if (diff) {
                if (!c.first_difference) {
                    c.first_difference = checkpoint_name(t) + ": " + *diff;
                }
            } else {
                ++c.equal;
            }
            if (c.replay->book().order_count() > c.most_orders) {
                c.most_orders = c.replay->book().order_count();
                c.most_at = t;
            }
        }
        std::cerr << "  " << checkpoint_name(t) << " compared (" << seconds_since(start) << " s)\n";
    }

    bool all_pass = true;
    for (Checked &c : stocks) {
        DepthReplay &r = *c.replay;
        r.finish();
        const bool pass = r.at_end() && !r.error() && !r.mismatch() && c.equal == times.size();
        all_pass = all_pass && pass;
        std::cout << c.symbol << " (locate " << c.locate << "): " << with_commas(r.stock_messages())
                  << " order messages; the reference agreed on " << with_commas(r.quick_checks())
                  << " quick and " << with_commas(r.full_checks()) << " full checks; equal to the "
                  << "full replay at " << c.equal << " of " << times.size() << " checkpoints (most "
                  << "resting: " << with_commas(c.most_orders) << " orders at "
                  << checkpoint_name(c.most_at) << ")";
        if (c.first_difference) {
            std::cout << "; first difference at " << *c.first_difference;
        }
        if (const auto &m = r.mismatch()) {
            std::cout << "; the reference differed at " << format_time(m->timestamp) << ": "
                      << m->what;
        }
        if (const auto &e = r.error()) {
            std::cout << "; broken message at byte " << e->offset;
        }
        std::cout << ": " << (pass ? "PASS" : "FAIL") << "\n";
    }
    return all_pass ? 0 : 1;
}

// The replay, its clock and what the user has chosen.
class Depth {
public:
    Depth(std::span<const std::byte> bytes, const itch::DayCounter &counter, std::uint16_t locate,
          const Options &o)
        : symbol_(std::string{view::trimmed(counter.directory()[locate])}),
          replay_(bytes, one_stock_configs(counter.directory(), counter.stats(), locate), locate,
                  ReplayOptions{.release_behind = true}),
          clock_(o.from, o.speed, now_ns()) {
        ui_.levels = o.levels;
        std::cerr << "replaying " << symbol_ << " up to " << format_time(o.from) << "...\n";
        replay_.advance_to(o.from);
        clock_ = ReplayClock{o.from, o.speed, now_ns()};
    }

    void tick() {
        if (!clock_.paused()) {
            replay_.advance_to(clock_.itch_now(now_ns()));
            if (!replay_.running()) {
                clock_.pause(now_ns()); // the end, or a difference: hold the screen there
            }
        }
    }

    bool key(const ftxui::Event &e, const ftxui::Closure &quit) {
        if (e == ftxui::Event::Character('q') || e == ftxui::Event::Escape) {
            quit();
        } else if (e == ftxui::Event::ArrowUp) {
            ui_.level -= ui_.level > 0 ? 1 : 0;
        } else if (e == ftxui::Event::ArrowDown) {
            if (ui_.level + 1 < replay_.levels(ui_.side, ui_.levels).size()) {
                ++ui_.level;
            }
        } else if (e == ftxui::Event::Tab) {
            ui_.side = ui_.side == Side::Buy ? Side::Sell : Side::Buy;
            ui_.level = 0;
        } else if (e == ftxui::Event::Character(' ')) {
            if (clock_.paused()) {
                clock_.resume(now_ns(), replay_.itch_time());
            } else {
                clock_.pause(now_ns());
            }
        } else if (e == ftxui::Event::Character('n')) {
            clock_.pause(now_ns());
            replay_.step();
        } else if (e == ftxui::Event::Character('f')) {
            follow_newest();
        } else if (e == ftxui::Event::Character('+') || e == ftxui::Event::Character('-')) {
            clock_.set_speed(now_ns(), e == ftxui::Event::Character('+') ? clock_.speed() * 2
                                                                         : clock_.speed() / 2);
        } else {
            return false;
        }
        return true;
    }

    [[nodiscard]] ftxui::Element draw() const {
        return render(make_frame(replay_, clock_, ui_, symbol_), ui_.levels,
                      depth_rows(ftxui::Terminal::Size().dimy, ui_.levels));
    }

private:
    // f: follow the newest order in the shown queue, the one with the longest way to go.
    void follow_newest() {
        if (replay_.follow_status()) {
            replay_.unfollow();
            return;
        }
        const std::vector<LevelRow> rows = replay_.levels(ui_.side, ui_.levels);
        if (rows.empty()) {
            return;
        }
        const std::vector<QueueRow> queue =
            replay_.queue(ui_.side, rows[std::min(ui_.level, rows.size() - 1)].price);
        if (!queue.empty()) {
            replay_.follow(queue.back().id);
        }
    }

    std::string symbol_;
    DepthReplay replay_;
    ReplayClock clock_;
    DepthUi ui_;
};

} // namespace

int main(int argc, char **argv) {
    const std::optional<Options> o = parse_args(argc, argv);
    if (!o) {
        std::cerr << kUsage;
        return 2;
    }
    const support::MappedFile file{o->file};
    const std::span<const std::byte> bytes = file.bytes();
    if (bytes.empty()) {
        std::cerr << "can't read " << o->file << "\n";
        return 1;
    }
    if (!o->check_day.empty()) {
        return check_day(bytes, *o);
    }

    const std::optional<itch::DayCounter> counter = count_day(bytes);
    if (!counter) {
        return 1;
    }
    const std::optional<std::uint16_t> locate = locate_of(*counter, o->symbol);
    if (!locate) {
        return 1;
    }
    Depth depth{bytes, *counter, *locate, *o};

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    const ftxui::Closure quit = screen.ExitLoopClosure();
    auto component = ftxui::CatchEvent(ftxui::Renderer([&] { return depth.draw(); }),
                                       [&](const ftxui::Event &e) {
                                           if (e == ftxui::Event::Custom) {
                                               depth.tick();
                                               return true;
                                           }
                                           return depth.key(e, quit);
                                       });
    // Declared after the screen, so it stops (and is joined) before the screen goes away.
    std::jthread ticker{[&screen](const std::stop_token &stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // depth design §3.3
            screen.PostEvent(ftxui::Event::Custom);
        }
    }};
    screen.Loop(component);
    ticker.request_stop();
    return 0;
}

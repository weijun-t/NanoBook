/*
 * nanobook_replay.cpp: replays a full ITCH 5.0 day into one book per stock and reports it.
 *
 * What it does: Two kinds of run.
 *   - Correctness (the default): counts what each stock needs, sizes every book, replays the
 *     whole day into FastBook books through the book builder, and prints messages by type, the
 *     result of every order message (Ok, UnknownOrder, ...), crossed books, the biggest books and
 *     the top levels of one stock. --check, --structure-every and --differential add the checks
 *     of ITCH design §8.1 and §8.2; --until stops at a time of day; --flow counts how many price
 *     levels each new or vanishing level shifts in the sorted array, and how far new orders are
 *     from the best price (support/itch_flow.hpp, step 6).
 *   - Timing (--throughput, --latency; step 6, design §9): messages per second for the parser
 *     alone and for parser + FastBook, and the latency of every Nth order message by kind, with
 *     the environment block and CSV files of the phase 3 harness.
 *   Exit code: 0 the day replayed (anomalies are reported, not fatal); 1 a bad file, a broken
 *   book structure, a book crossed while its stock was trading (design §8.1) or (--differential)
 *   a difference between the reference and fast books; 2 bad arguments; 3 no usable timer.
 *     nanobook_replay FILE [--symbol AAPL] [--depth N] [--until HH:MM:SS] [--check]
 *                     [--structure-every N] [--differential] [--flow]
 *     nanobook_replay FILE [--throughput] [--latency] [--every N] [--window-mb N] [--core C]
 *                     [--csv FILE.csv]
 *
 * Why it exists: Phase 4's exit criteria (ITCH design §1): a full day replays, the consistency
 *   checks pass, and messages per second and per-message latency are reported.
 *
 * How it works:
 *   - Correctness: mmap the file read-only, then counting (day_stats.hpp), sizing and building the
 *     books (sizing.hpp), replaying (book_builder.hpp), each timed with the system clock. Pages
 *     are read from disk as needed, so those times include the disk: they are not benchmarks.
 *   - Timing: the file is read in windows (support/itch_timing.hpp): each window's pages are
 *     loaded before the clock starts and released after, so only parsing and book work are
 *     timed, and memory stays near one window. Checks are off. The time-stamp counter is
 *     calibrated, its own cost subtracted from every latency sample, and the environment printed
 *     next to the numbers (rules 19, 20), as in nanobook_bench.
 *
 * Trade-offs: A tool, not the engine: it prints, allocates and reads the clock freely, all
 *   outside the timed regions.
 */

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <expected>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nanobook/core/names.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/reference/reference_book.hpp"

#include "environment.hpp" // support/: test and tool code
#include "itch_flow.hpp"
#include "itch_replay_comparison.hpp"
#include "itch_timing.hpp"
#include "latency_recorder.hpp"
#include "tsc.hpp"

namespace {

using namespace nanobook;
using namespace nanobook::itch;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kUsage =
    "usage: nanobook_replay FILE [--symbol AAPL] [--depth N] [--until HH:MM:SS] [--check]\n"
    "                       [--structure-every N] [--differential] [--flow]\n"
    "       nanobook_replay FILE [--throughput] [--latency] [--every N] [--window-mb N] [--core "
    "C]\n"
    "                       [--csv FILE.csv]\n";

struct Options {
    std::string file;
    std::string symbol;
    std::size_t depth{10};
    std::uint64_t until{~std::uint64_t{0}}; // nanoseconds since midnight; default: the whole day
    bool check{false};
    std::uint64_t structure_every{0};
    bool differential{false}; // also replay into the reference book and compare (design §8.2)
    bool flow{false};         // count level changes, their shifts, and distances from the best
    // Timing (step 6)
    bool throughput{false};
    bool latency{false};
    std::uint64_t every{8}; // --latency times every Nth order message (design §13 Q5)
    std::size_t window_mb{1024};
    int core{-1};
    std::string csv;
};

// Reads a whole decimal number; false if `text` isn't one.
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
    ns = ((h * 60 + m) * 60 + s) * 1'000'000'000;
    return true;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--check") {
            o.check = true;
            continue;
        }
        if (arg == "--differential") {
            o.differential = true;
            continue;
        }
        if (arg == "--flow") {
            o.flow = true;
            continue;
        }
        if (arg == "--throughput" || arg == "--latency") {
            (arg == "--throughput" ? o.throughput : o.latency) = true;
            continue;
        }
        if (!arg.starts_with("--")) {
            if (!o.file.empty()) {
                return std::nullopt; // two files
            }
            o.file = arg;
            continue;
        }
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        bool ok = false;
        if (arg == "--symbol") {
            o.symbol = value;
            ok = !value.empty() && value.size() <= 8;
        } else if (arg == "--depth") {
            ok = parse_number(value, o.depth);
        } else if (arg == "--until") {
            ok = parse_time(value, o.until);
        } else if (arg == "--structure-every") {
            ok = parse_number(value, o.structure_every);
            o.check = true;
        } else if (arg == "--every") {
            ok = parse_number(value, o.every) && o.every > 0;
        } else if (arg == "--window-mb") {
            ok = parse_number(value, o.window_mb) && o.window_mb > 0;
        } else if (arg == "--core") {
            ok = parse_number(value, o.core) && o.core >= 0;
        } else if (arg == "--csv") {
            o.csv = value;
            ok = !o.csv.empty();
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    if (o.file.empty()) {
        return std::nullopt;
    }
    return o;
}

// The file, mapped read-only. Unmapped when it goes out of scope.
class MappedFile {
public:
    explicit MappedFile(const std::string &path) {
        const int fd = open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) {
            if (fd >= 0) {
                close(fd);
            }
            return;
        }
        size_ = static_cast<std::size_t>(st.st_size);
        void *map = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd); // the mapping keeps the file open
        if (map == MAP_FAILED) {
            size_ = 0;
            return;
        }
        madvise(map, size_, MADV_SEQUENTIAL); // read ahead, drop pages behind
        data_ = static_cast<const std::byte *>(map);
    }
    ~MappedFile() {
        if (data_ != nullptr) {
            munmap(const_cast<std::byte *>(data_), size_);
        }
    }
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    MappedFile(MappedFile &&) = delete;
    MappedFile &operator=(MappedFile &&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const {
        return {data_, size_};
    }

private:
    const std::byte *data_{nullptr};
    std::size_t size_{0};
};

// Passes messages to `inner` up to the first one stamped after `until`, then drops the rest
// (skipped types carry no decoded time, so they're dropped from that point on too).
template <typename Inner> class Until {
public:
    Until(Inner &inner, std::uint64_t until) : inner_(inner), until_(until) {
    }
    template <typename Message> void on(const Message &m) {
        stopped_ = stopped_ || m.timestamp > until_;
        if (stopped_) {
            ++dropped_;
            return;
        }
        inner_.on(m);
    }
    void on_skipped(char type) {
        if (stopped_) {
            ++dropped_;
            return;
        }
        inner_.on_skipped(type);
    }
    [[nodiscard]] std::uint64_t dropped() const {
        return dropped_;
    }

private:
    Inner &inner_;
    std::uint64_t until_;
    bool stopped_{false};
    std::uint64_t dropped_{};
};

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string trimmed(const Symbol &s) {
    std::string text(s.begin(), s.end());
    text.erase(text.find_last_not_of(" \0", std::string::npos, 2) + 1);
    return text;
}

Symbol to_symbol(std::string_view text) {
    Symbol s;
    s.fill(' ');
    std::copy_n(text.begin(), std::min(text.size(), s.size()), s.begin());
    return s;
}

std::string time_of_day(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000;
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60),
                  static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000));
    return text.data();
}

std::string dollars(Price p) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%lld.%04lld", static_cast<long long>(p.ticks / 10'000),
                  static_cast<long long>(p.ticks % 10'000));
    return text.data();
}

// Is this parse result the whole buffer? Prints why not.
bool parsed_whole(const std::expected<std::size_t, ParseError> &result, std::size_t size,
                  std::string_view pass) {
    if (!result) {
        const ParseError &e = result.error();
        std::cerr << pass << ": bad message at byte " << e.offset << " (type '" << e.type
                  << "', length " << e.length << ")\n";
        return false;
    }
    if (*result != size) {
        std::cerr << pass << ": the file ends in the middle of a message, at byte " << *result
                  << "\n";
        return false;
    }
    return true;
}

void print_messages(const ReplayStats &s) {
    std::uint64_t total = 0;
    std::cout << "\nMessages by type\n";
    for (std::size_t type = 0; type < s.messages.size(); ++type) {
        if (s.messages[type] != 0) {
            std::cout << "  " << static_cast<char>(type) << std::setw(14) << s.messages[type]
                      << "\n";
            total += s.messages[type];
        }
    }
    std::cout << "  all" << std::setw(12) << total << "\n";
}

void print_results(const ReplayStats &s) {
    constexpr std::array<std::pair<char, OrderMessage>, kOrderMessages> kRows{{
        {'A', OrderMessage::Add},
        {'F', OrderMessage::AddMpid},
        {'E', OrderMessage::Executed},
        {'C', OrderMessage::ExecutedWithPrice},
        {'X', OrderMessage::Cancel},
        {'D', OrderMessage::Delete},
        {'U', OrderMessage::Replace},
    }};
    std::cout << "\nResult of every order message\n     ";
    for (std::size_t r = 0; r < kFeedResults; ++r) {
        std::cout << std::setw(15) << name(static_cast<FeedResult>(r));
    }
    std::cout << "\n";
    for (const auto &[letter, kind] : kRows) {
        std::cout << "  " << letter << "  ";
        for (std::size_t r = 0; r < kFeedResults; ++r) {
            std::cout << std::setw(15) << s.result(kind, static_cast<FeedResult>(r));
        }
        std::cout << "\n";
    }
    std::cout << "  order messages for a locate no R named: " << s.unknown_locate << "\n"
              << "  bad input (ref 0, or side not B/S):     " << s.bad_input << "\n";
}

template <typename Builder> void print_checks(const Builder &builder, const Options &o) {
    const ReplayStats &s = builder.stats();
    if (!o.check) {
        std::cout << "\nChecks: off (use --check)\n";
        return;
    }
    std::cout << "\nCrossed books (best bid >= best ask)\n"
              << "  times a book became crossed:        " << s.crossed_starts << "\n"
              << "  ...while its stock was trading (T): " << s.crossed_while_trading
              << "  (must be 0, design §8.1)\n"
              << "  order messages leaving it crossed:  " << s.crossed_messages << "\n";
    const std::size_t shown = std::min<std::uint64_t>(s.crossed_starts, s.first_crossed.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const CrossedMoment &m = s.first_crossed[i];
        std::cout << "    " << time_of_day(m.timestamp) << "  " << std::left << std::setw(8)
                  << trimmed(builder.name(m.locate)) << std::right << "  trading state "
                  << m.trading_state << ", last system event " << m.system_event << "\n";
    }
    if (o.structure_every != 0) {
        std::cout << "Book structure: " << s.structure_checks << " checks, " << s.structure_failures
                  << " failures\n";
    }
}

template <typename Builder> void print_biggest(const Builder &builder) {
    std::vector<std::pair<std::size_t, std::uint16_t>> sizes; // (resting orders, locate)
    std::size_t total = 0;
    for (std::size_t locate = 1; locate < builder.locates(); ++locate) {
        const std::size_t n = builder.book(static_cast<std::uint16_t>(locate)).order_count();
        total += n;
        sizes.emplace_back(n, static_cast<std::uint16_t>(locate));
    }
    std::ranges::sort(sizes, std::greater<>{});
    std::cout << "\nOrders resting at the end of the replay: " << total << "; biggest books:\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, sizes.size()); ++i) {
        std::cout << "  " << std::left << std::setw(8) << trimmed(builder.name(sizes[i].second))
                  << std::right << std::setw(10) << sizes[i].first << "\n";
    }
}

void print_peaks(const DayCounter &counter) {
    std::vector<std::pair<std::uint32_t, Symbol>> peaks; // (peak resting orders, symbol)
    for (const auto &[symbol, stats] : counter.stats()) {
        peaks.emplace_back(stats.peak_orders, symbol);
    }
    std::ranges::sort(peaks, std::greater<>{});
    std::cout << "\nMost orders resting at once during the day:\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, peaks.size()); ++i) {
        std::cout << "  " << std::left << std::setw(8) << trimmed(peaks[i].second) << std::right
                  << std::setw(10) << peaks[i].first << "\n";
    }
}

// The best `depth` levels of each side.
void print_depth(const FastBook &book, std::size_t depth) {
    for (const Side side : {Side::Sell, Side::Buy}) {
        std::vector<std::pair<Price, Qty>> levels = book.depth(side, depth);
        if (side == Side::Sell) {
            std::ranges::reverse(levels); // asks above bids, best in the middle
        }
        for (const auto &[price, qty] : levels) {
            std::cout << "  " << (side == Side::Sell ? "ask " : "bid ") << std::setw(12)
                      << dollars(price) << std::setw(10) << qty.value << "\n";
        }
        if (side == Side::Sell) {
            std::cout << "  ----\n";
        }
    }
}

// The flow analysis (support/itch_flow.hpp): levels created and removed by how many levels they
// shifted, and new orders by distance from the best price.
void print_flow(const FlowStats &s) {
    const auto table = [](std::string_view what,
                          const std::array<std::uint64_t, kShiftBuckets> &counts,
                          std::uint64_t shifted) {
        std::uint64_t total = 0;
        for (const std::uint64_t n : counts) {
            total += n;
        }
        std::cout << "\n"
                  << what << ": " << total << ", shifting " << shifted << " levels in all ("
                  << std::setprecision(1) << std::fixed << static_cast<double>(shifted) * 32 / 1e9
                  << " GB moved at 32 bytes each)\n"
                  << "  levels shifted        count   share  cumulative\n";
        std::uint64_t so_far = 0;
        for (std::size_t b = 0; b < counts.size(); ++b) {
            if (counts[b] == 0) {
                continue;
            }
            so_far += counts[b];
            const std::uint64_t low = b == 0 ? 0 : std::uint64_t{1} << (b - 1);
            const std::uint64_t high = b == 0 ? 0 : (std::uint64_t{1} << b) - 1;
            const std::string range = low == high
                                          ? std::to_string(low)
                                          : std::to_string(low) + "-" + std::to_string(high);
            std::cout << "  " << std::left << std::setw(16) << range << std::right << std::setw(12)
                      << counts[b] << std::setw(7) << std::setprecision(2)
                      << 100.0 * static_cast<double>(counts[b]) / static_cast<double>(total) << "%"
                      << std::setw(10)
                      << 100.0 * static_cast<double>(so_far) / static_cast<double>(total) << "%\n";
        }
    };
    table("Price levels created", s.created, s.created_shifted);
    table("Price levels removed", s.removed, s.removed_shifted);

    constexpr std::array<std::string_view, kDistances> kNames{"side was empty", "improves the best",
                                                              "at the best",    "behind by < $0.01",
                                                              "< $0.10",        "< $1",
                                                              "< $10",          "< $100",
                                                              "$100 or more"};
    std::uint64_t total = 0;
    for (const std::uint64_t n : s.distance) {
        total += n;
    }
    std::cout << "\nNew resting orders by distance from their side's best price: " << total << "\n";
    for (std::size_t d = 0; d < kDistances; ++d) {
        std::cout << "  " << std::left << std::setw(20) << kNames[d] << std::right << std::setw(12)
                  << s.distance[d] << std::setw(7) << std::setprecision(2)
                  << 100.0 * static_cast<double>(s.distance[d]) / static_cast<double>(total)
                  << "%\n";
    }
}

// ---------------------------------------------------------------------------
// Timing (step 6, ITCH design §9)
// ---------------------------------------------------------------------------

// Moves this program onto `core` and checks that it is running there (as nanobook_bench).
bool pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 && sched_getcpu() == core;
}

// "out.csv" -> "out_histogram.csv", the pair nanobook_bench writes and plot_latency.py reads.
std::string histogram_path(std::string_view csv) {
    const std::string_view stem = csv.ends_with(".csv") ? csv.substr(0, csv.size() - 4) : csv;
    return std::string{stem} + "_histogram.csv";
}

// The ITCH messages behind each tag (support/itch_timing.hpp).
std::string_view itch_kinds(latency::Tag tag) {
    switch (tag) {
    case latency::Tag::Add:
        return "A, F";
    case latency::Tag::Match:
        return "E, C";
    case latency::Tag::Cancel:
        return "D";
    case latency::Tag::Modify:
        return "X";
    case latency::Tag::Replace:
        return "U";
    case latency::Tag::Reject:
    case latency::Tag::Other:
        break;
    }
    return "";
}

void print_ns(double ns) {
    std::cout << std::setw(10) << std::fixed << std::setprecision(ns < 1'000 ? 1 : 0) << ns;
}

struct CsvRow {
    std::string mode; // "throughput" or "latency"
    std::string book; // "parse_only" or "FastBook"
    std::string tag;
    latency::Summary summary;
};

int run_timing(const Options &o, std::span<const std::byte> bytes) {
    using namespace nanobook::latency;
    const auto missing = missing_tsc_flags(read_cpu_flags());
    if (!missing.empty()) {
        std::cerr << "the time-stamp counter can't be used as a clock here\n";
        return 3;
    }
    if (o.core >= 0 && !pin_to_core(o.core)) {
        std::cerr << "could not pin to core " << o.core << "\n";
        return 3;
    }
    const Environment env = collect_environment(o.core);
    const Calibration calibration = calibrate();
    const Overhead overhead = measure_overhead();
    const double rate = calibration.ticks_per_ns;
    const std::string banner = "*** NOT FOR PUBLISHING: " + env.unpublishable_reason() + " ***";
    if (!env.publishable()) {
        std::cout << banner << "\n";
    }
    for (const auto &[key, value] : env.lines()) {
        std::cout << std::left << std::setw(17) << key << value << "\n";
    }
    std::cout << std::setw(17) << "pinned to core" << (o.core >= 0 ? std::to_string(o.core) : "no")
              << "\n"
              << std::setw(17) << "counter" << std::fixed << std::setprecision(4) << rate
              << " ticks per ns\n"
              << std::setw(17) << "timer overhead" << "min " << overhead.min
              << " ticks (subtracted from every latency sample)\n"
              << std::setw(17) << "windows" << o.window_mb
              << " MiB, loaded before the clock starts\n"
              << std::right;

    const std::size_t window = o.window_mb << 20U;
    const auto failed = [&](const auto &run, std::string_view pass) {
        if (!run) {
            std::cerr << pass << ": bad message at byte " << run.error().offset << "\n";
            return true;
        }
        if (run->bytes != bytes.size()) {
            std::cerr << pass << ": the file ends in the middle of a message\n";
            return true;
        }
        return false;
    };

    // Counting and sizing (untimed; in windows too, so memory stays near one window).
    DayCounter counter{1 << 22};
    if (failed(replay_in_windows(bytes, window, counter), "counting")) {
        return 1;
    }
    const std::vector<BookConfig> configs =
        size_books(counter.directory(), counter.stats(), SizingRules{});

    std::vector<CsvRow> rows;
    std::vector<std::pair<std::string, Bucket>> histogram_rows;

    if (o.throughput) {
        ParseOnly parse_only;
        const auto parsed = replay_in_windows(bytes, window, parse_only);
        if (failed(parsed, "parse only")) {
            return 1;
        }
        BookBuilder<FastBook> builder{configs, BuilderOptions{}};
        const auto replayed = replay_in_windows(bytes, window, builder);
        if (failed(replayed, "replay")) {
            return 1;
        }
        const auto messages = static_cast<double>(parse_only.messages);
        const double parse_ns = to_ns(parsed->timed_ticks, rate);
        const double replay_ns = to_ns(replayed->timed_ticks, rate);
        std::cout << "\nThroughput (checks off; " << parse_only.messages << " messages, "
                  << replayed->windows << " windows; checksum " << parse_only.sum << ")\n"
                  << std::setprecision(2) << "  parse only:        " << parse_ns / 1e9 << " s, "
                  << std::setprecision(1) << parse_ns / messages << " ns per message, "
                  << messages / parse_ns * 1e3 << " M messages/s\n"
                  << std::setprecision(2) << "  parse + FastBook:  " << replay_ns / 1e9 << " s, "
                  << std::setprecision(1) << replay_ns / messages << " ns per message, "
                  << messages / replay_ns * 1e3 << " M messages/s\n"
                  << "  the books' share:  " << (replay_ns - parse_ns) / messages
                  << " ns per message (" << std::setprecision(0)
                  << 100.0 * (replay_ns - parse_ns) / replay_ns << "% of the time)\n";
        rows.push_back({"throughput", "parse_only", "all",
                        Summary{.count = parse_only.messages, .mean_ns = parse_ns / messages}});
        rows.push_back({"throughput", "FastBook", "all",
                        Summary{.count = parse_only.messages, .mean_ns = replay_ns / messages}});
    }

    if (o.latency) {
        // Room for every Nth of the most order messages the file could hold (21 bytes each).
        Recorder recorder{bytes.size() / (kLengthPrefix + 19) / o.every + 1};
        BookBuilder<FastBook> builder{configs, BuilderOptions{}};
        TimedOrders timed{builder, recorder, o.every};
        if (failed(replay_in_windows(bytes, window, timed), "latency")) {
            return 1;
        }
        std::cout << "\nLatency of every " << o.every << "th order message, FastBook ("
                  << recorder.size() << " samples; ns, timer overhead subtracted)\n"
                  << "tag      ITCH       count       min       p50       p90       p99     p99.9"
                     "    p99.99       max      mean\n";
        std::vector<std::pair<std::string, std::optional<Tag>>> groups;
        for (const Tag tag : kAllTags) {
            if (std::ranges::find(recorder.tags(), tag) != recorder.tags().end()) {
                groups.emplace_back(std::string{tag_name(tag)}, tag);
            }
        }
        groups.emplace_back("all", std::nullopt);
        for (const auto &[label, tag] : groups) {
            const Summary s = summarize(recorder, overhead.min, rate, tag);
            std::cout << std::left << std::setw(9) << label << std::setw(6)
                      << (tag ? itch_kinds(*tag) : "") << std::right << std::setw(10) << s.count;
            for (const double v : {s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns, s.p999_ns}) {
                print_ns(v);
            }
            if (s.count >= 1'000'000) { // p99.99 rests on at least 100 samples
                print_ns(s.p9999_ns);
            } else {
                std::cout << std::setw(10) << "-";
            }
            print_ns(s.max_ns);
            print_ns(s.mean_ns);
            std::cout << "\n";
            rows.push_back({"latency", "FastBook", label, s});
            if (!o.csv.empty()) {
                for (const Bucket &b :
                     histogram(sorted_samples(recorder, overhead.min, tag), rate)) {
                    histogram_rows.emplace_back(label, b);
                }
            }
        }
        std::cout << "  dropped (moved core) " << recorder.dropped() << ", clamped (too long) "
                  << recorder.clamped() << ", refused (full) " << recorder.refused() << "\n";
    }

    const Calibration after = calibrate();
    const double drift = (after.ticks_per_ns - rate) / rate;
    std::cout << "\ncounter drift during the run: " << std::setprecision(3) << std::abs(drift) * 1e6
              << " parts per million\n";

    if (!o.csv.empty()) {
        std::ofstream summary{o.csv};
        std::ofstream hist{histogram_path(o.csv)};
        if (!summary || !hist) {
            std::cerr << "can't write " << o.csv << "\n";
            return 2;
        }
        for (std::ostream *f :
             {static_cast<std::ostream *>(&summary), static_cast<std::ostream *>(&hist)}) {
            if (!env.publishable()) {
                *f << "# " << banner << "\n";
            }
            for (const auto &[key, value] : env.lines()) {
                *f << "# " << key << ": " << value << "\n";
            }
            *f << std::fixed
               << "# pinned to core: " << (o.core >= 0 ? std::to_string(o.core) : "no") << "\n"
               << std::setprecision(6) << "# counter ticks per ns: " << rate << "\n"
               << "# counter drift during the run (parts per million): " << drift * 1e6 << "\n"
               << "# timer overhead min (ticks, subtracted): " << overhead.min << "\n"
               << "# input: " << o.file << "\n"
               << "# latency: every " << o.every << "th order message; windows of " << o.window_mb
               << " MiB\n";
        }
        summary << "mode,scenario,book,tag,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,"
                   "max_ns,mean_ns,dropped,clamped,timed_allocations\n"
                << std::setprecision(1);
        for (const CsvRow &r : rows) {
            const Summary &s = r.summary;
            summary << r.mode << ",itch_day," << r.book << "," << r.tag << "," << s.count << ",";
            if (r.mode == "latency") {
                summary << s.min_ns << "," << s.p50_ns << "," << s.p90_ns << "," << s.p99_ns << ","
                        << s.p999_ns << "," << s.p9999_ns << "," << s.max_ns << ",";
            } else {
                summary << ",,,,,,,";
            }
            summary << s.mean_ns << ",,,\n";
        }
        hist << "scenario,book,tag,low_ns,high_ns,count\n";
        for (const auto &[tag, b] : histogram_rows) {
            hist << "itch_day,FastBook," << tag << "," << b.low_ns << "," << b.high_ns << ","
                 << b.count << "\n";
        }
        std::cout << "wrote " << o.csv << " and " << histogram_path(o.csv) << "\n";
    }
    if (!env.publishable()) {
        std::cout << banner << "\n";
    }
    return 0;
}

long max_rss_mb() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024; // KB on Linux
}

} // namespace

int main(int argc, char **argv) {
    const std::optional<Options> parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << kUsage;
        return 2;
    }
    const Options &o = *parsed;
    const MappedFile file{o.file};
    const std::span<const std::byte> bytes = file.bytes();
    if (bytes.empty()) {
        std::cerr << "can't read " << o.file << "\n";
        return 1;
    }
    std::cout << "File: " << o.file << " (" << bytes.size() << " bytes)\n";
    if (o.throughput || o.latency) {
        return run_timing(o, bytes);
    }

    // 1. Counting (ITCH design §7).
    auto start = Clock::now();
    DayCounter counter{1 << 22};
    if (!parsed_whole(parse(bytes, counter), bytes.size(), "counting")) {
        return 1;
    }
    std::cout << "Counting pass:  " << std::fixed << std::setprecision(1) << seconds_since(start)
              << " s (" << counter.directory().size() - 1 << " locates; " << counter.ignored()
              << " inputs the counter couldn't follow)\n";

    // 2. Sizing and building the books.
    start = Clock::now();
    const std::vector<BookConfig> configs =
        size_books(counter.directory(), counter.stats(), SizingRules{});
    std::uint64_t room = 0;
    for (const BookConfig &c : configs) {
        room += c.max_orders;
    }
    BookBuilder<FastBook> builder{
        configs, BuilderOptions{.check = o.check, .structure_every = o.structure_every}};
    std::cout << "Building books: " << seconds_since(start) << " s (room for " << room
              << " resting orders in all)\n";

    // 3. The replay, into the fast books alone or (--differential) into the reference books too.
    start = Clock::now();
    const auto replay = [&](auto &handler) {
        Until until{handler, o.until};
        if (!parsed_whole(parse(bytes, until), bytes.size(), "replay")) {
            return false;
        }
        if (until.dropped() != 0) {
            std::cout << "Stopped at " << time_of_day(o.until) << ": " << until.dropped()
                      << " later messages not replayed\n";
        }
        return true;
    };
    std::uint64_t mismatches = 0;
    if (o.differential) {
        BookBuilder<ReferenceBook> reference{
            configs, BuilderOptions{.check = o.check, .structure_every = o.structure_every}};
        ReplayComparison comparison{reference, builder, 1'000'000};
        if (!replay(comparison)) {
            return 1;
        }
        comparison.finish();
        mismatches = comparison.mismatches();
        std::cout << "Replay:         " << seconds_since(start) << " s, reference and fast books"
                  << (o.check ? ", checks on" : "") << "\n"
                  << "Comparison:     " << comparison.quick_checks() << " quick checks, "
                  << comparison.full_checks() << " full book checks, " << mismatches
                  << " mismatches\n";
        if (const auto &first = comparison.first_mismatch()) {
            std::cout << "  first at message " << first->message << " ("
                      << time_of_day(first->timestamp) << "), locate " << first->locate << ": "
                      << first->what << "\n";
        }
    } else if (o.flow) {
        FlowAnalysis flow{builder};
        if (!replay(flow)) {
            return 1;
        }
        std::cout << "Replay:         " << seconds_since(start) << " s, with the flow analysis\n";
        print_flow(flow.stats());
    } else {
        if (!replay(builder)) {
            return 1;
        }
        std::cout << "Replay:         " << seconds_since(start) << " s"
                  << (o.check ? ", checks on" : "") << "\n";
    }

    print_messages(builder.stats());
    print_results(builder.stats());
    print_checks(builder, o);
    print_peaks(counter);
    print_biggest(builder);

    if (!o.symbol.empty()) {
        const std::uint16_t locate = builder.find(to_symbol(o.symbol));
        if (locate == 0) {
            std::cout << "\nNo stock named " << o.symbol << "\n";
        } else {
            std::cout << "\n"
                      << o.symbol << " at the end of the replay (locate " << locate << "), top "
                      << o.depth << " levels:\n";
            print_depth(builder.book(locate), o.depth);
        }
    }
    std::cout << "\nPeak memory: " << max_rss_mb() << " MB (includes the file's pages read)\n";
    const ReplayStats &s = builder.stats();
    return s.structure_failures == 0 && s.crossed_while_trading == 0 && mismatches == 0 ? 0 : 1;
}

/*
 * nanobook_replay.cpp: replays a full ITCH 5.0 day into one book per stock and reports it.
 *
 * What it does: Reads an uncompressed ITCH file, counts what each stock needs, sizes every book,
 *   replays the whole day into FastBook books through the book builder, and prints: messages by
 *   type, the result of every order message (Ok, UnknownOrder, ...), crossed books, the biggest
 *   books, and the top levels of one stock. Exit code 0 means the day replayed (anomalies are
 *   reported, not fatal), 1 a bad file, a broken book structure or a book crossed while its
 *   stock was trading (design §8.1), 2 bad arguments.
 *     nanobook_replay FILE [--symbol AAPL] [--depth N] [--until HH:MM:SS] [--check]
 *                     [--structure-every N]
 *
 * Why it exists: Phase 4's exit criteria (ITCH design §1): a full day replays and the
 *   consistency checks pass. This is the minimal version (step 4b); step 6 adds pre-faulted
 *   windows and timing, and the CSV output.
 *
 * How it works: mmap the file read-only, then three steps, each timed with the system clock:
 *   counting (day_stats.hpp), building the books (sizing.hpp), replaying (book_builder.hpp).
 *   --check watches every order message for a crossed book; --structure-every N also checks a
 *   book's structure every N order messages. --until stops the replay at a time of day, so the
 *   books can be seen mid-day (at the end of the day Nasdaq has deleted every order). Pages are
 * read from disk as needed, so the times include the disk: they are not benchmarks (that's step 6).
 *
 * Trade-offs: A tool, not the engine: it prints, allocates and reads the clock freely, all
 *   outside the replay loop.
 */

#include <fcntl.h>
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
#include <cstdio>
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

namespace {

using namespace nanobook;
using namespace nanobook::itch;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kUsage =
    "usage: nanobook_replay FILE [--symbol AAPL] [--depth N] [--until HH:MM:SS] [--check]\n"
    "                       [--structure-every N]\n";

struct Options {
    std::string file;
    std::string symbol;
    std::size_t depth{10};
    std::uint64_t until{~std::uint64_t{0}}; // nanoseconds since midnight; default: the whole day
    bool check{false};
    std::uint64_t structure_every{0};
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

// The best `depth` levels of each side, walking price by price ($0.0001) from the best one.
void print_depth(const FastBook &book, const BookConfig &config, std::size_t depth) {
    for (const Side side : {Side::Sell, Side::Buy}) {
        std::vector<std::pair<Price, Qty>> levels;
        const std::optional<Price> best = side == Side::Buy ? book.best_bid() : book.best_ask();
        const std::int64_t step = side == Side::Buy ? -1 : 1;
        for (Price p = best.value_or(Price{0}); best && levels.size() < depth && config.in_band(p);
             p = Price{p.ticks + step}) {
            const Qty qty = book.level_qty(side, p);
            if (qty > Qty{0}) {
                levels.emplace_back(p, qty);
            }
        }
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

    // 3. The replay.
    start = Clock::now();
    Until<BookBuilder<FastBook>> until{builder, o.until};
    if (!parsed_whole(parse(bytes, until), bytes.size(), "replay")) {
        return 1;
    }
    std::cout << "Replay:         " << seconds_since(start) << " s"
              << (o.check ? ", checks on" : "") << "\n";
    if (until.dropped() != 0) {
        std::cout << "Stopped at " << time_of_day(o.until) << ": " << until.dropped()
                  << " later messages not replayed\n";
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
            print_depth(builder.book(locate), configs[locate], o.depth);
        }
    }
    std::cout << "\nPeak memory: " << max_rss_mb() << " MB (includes the file's pages read)\n";
    const ReplayStats &s = builder.stats();
    return s.structure_failures == 0 && s.crossed_while_trading == 0 ? 0 : 1;
}

/*
 * nanobook_pipeline.cpp: replays an ITCH day through the three-thread pipeline into a binary log.
 *
 * What it does: Counts what each stock needs and sizes the books (phase 4), then replays the day
 *   through the pipeline (feed, engine and publisher threads; pipeline design §2), or with
 *   --sequential the same work on one thread, writing every TopOfBook update to a binary log
 *   (--log FILE; /dev/null if none). Prints the records sent, updates published, how often each
 *   ring was full, the wall time, and the log's size and checksum (--checksum). Exit code 0 if
 *   the whole day was read and the log written; 1 otherwise; 2 bad arguments.
 *     nanobook_pipeline FILE [--log OUT] [--checksum] [--sequential] [--cores A,B,C]
 *                       [--window-mb N]
 *
 * Why it exists: Step 3 proves the pipeline changes nothing: the same day must give the same log,
 *   byte for byte, on three threads and on one (rule 11). Comparing checksums proves it without
 *   keeping two 20 GB files (WSL2's disk grows but never shrinks on its own). Step 4 adds the
 *   timing modes.
 *
 * How it works: The file is mapped (support/mapped_file.hpp) and read in pre-loaded windows
 *   (support/itch_timing.hpp) by whichever thread parses: the feed thread, or the only thread.
 *   The publisher's sink is the BinaryLog. --cores pins the three threads (feed, engine,
 *   publisher) to three cores. The wall time is the system clock's, for orientation only: this
 *   is a correctness run, not a benchmark.
 *
 * Trade-offs: A tool: it prints and allocates freely outside the pipeline's run.
 */

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "binary_log.hpp" // support/
#include "itch_timing.hpp"
#include "mapped_file.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/pipeline/stages.hpp"

namespace {

using namespace nanobook;
using namespace nanobook::pipeline;

constexpr std::string_view kUsage =
    "usage: nanobook_pipeline FILE [--log OUT] [--checksum] [--sequential] [--cores A,B,C]\n"
    "                         [--window-mb N]\n";

struct Options {
    std::string file;
    std::string log{"/dev/null"};
    bool checksum{false};
    bool sequential{false};
    PipelineOptions pipeline{};
    std::size_t window_mb{1024};
};

template <typename T> bool parse_number(std::string_view text, T &value) {
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

// "2,4,6" -> feed, engine and publisher cores.
bool parse_cores(std::string_view text, PipelineOptions &p) {
    std::array<int, 3> cores{};
    for (std::size_t i = 0; i < cores.size(); ++i) {
        const std::size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        if (!parse_number(part, cores[i]) || cores[i] < 0 ||
            (i < 2) == (comma == std::string_view::npos)) {
            return false;
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    p.feed_core = cores[0];
    p.engine_core = cores[1];
    p.publish_core = cores[2];
    return true;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--checksum" || arg == "--sequential") {
            (arg == "--checksum" ? o.checksum : o.sequential) = true;
            continue;
        }
        if (!arg.starts_with("--")) {
            if (!o.file.empty()) {
                return std::nullopt;
            }
            o.file = arg;
            continue;
        }
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        bool ok = false;
        if (arg == "--log") {
            o.log = value;
            ok = !o.log.empty();
        } else if (arg == "--cores") {
            ok = parse_cores(value, o.pipeline);
        } else if (arg == "--window-mb") {
            ok = parse_number(value, o.window_mb) && o.window_mb > 0;
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

} // namespace

int main(int argc, char **argv) {
    const std::optional<Options> parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << kUsage;
        return 2;
    }
    const Options &o = *parsed;
    const support::MappedFile file{o.file};
    const std::span<const std::byte> bytes = file.bytes();
    if (bytes.empty()) {
        std::cerr << "can't read " << o.file << "\n";
        return 1;
    }
    const std::size_t window = o.window_mb << 20U;

    // Size the books (phase 4 §7), in windows so memory stays near one window.
    itch::DayCounter counter{1 << 22};
    const auto counted = itch::replay_in_windows(bytes, window, counter);
    if (!counted || counted->bytes != bytes.size()) {
        std::cerr << "counting: the file is broken or ends in the middle of a message\n";
        return 1;
    }
    itch::BookBuilder<FastBook> builder{
        itch::size_books(counter.directory(), counter.stats(), itch::SizingRules{}),
        itch::BuilderOptions{}};
    BinaryLog log{o.log, o.checksum};
    if (!log.ok()) {
        std::cerr << "can't write " << o.log << "\n";
        return 1;
    }
    const auto read_feed = [&](auto &handler) {
        const auto run = itch::replay_in_windows(bytes, window, handler);
        return run.has_value() && run->bytes == bytes.size();
    };

    const auto start = std::chrono::steady_clock::now();
    const PipelineResult r = o.sequential ? run_sequential(read_feed, builder, log)
                                          : run_pipeline(read_feed, builder, log, o.pipeline);
    log.close();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "File:       " << o.file << " (" << bytes.size() << " bytes)\n"
              << "Mode:       " << (o.sequential ? "one thread" : "pipeline, three threads");
    if (!o.sequential && o.pipeline.feed_core >= 0) {
        std::cout << " on cores " << o.pipeline.feed_core << ", " << o.pipeline.engine_core << ", "
                  << o.pipeline.publish_core
                  << (r.feed_pinned && r.engine_pinned && r.publish_pinned ? "" : " (NOT pinned)");
    }
    std::cout << "\nRecords:    " << r.records << " sent to the engine, " << r.skipped
              << " messages the books don't use\n"
              << "Updates:    " << r.updates << " TopOfBook updates published\n";
    if (!o.sequential) {
        std::cout << "Waits:      ring 1 full " << r.feed_waits << " times, ring 2 full "
                  << r.engine_waits << " times\n";
    }
    std::cout << "Log:        " << o.log << ", " << log.records() << " records, " << log.bytes()
              << " bytes";
    if (o.checksum) {
        std::cout << ", checksum " << std::hex << log.checksum() << std::dec;
    }
    std::cout << "\nWall time:  " << std::fixed << std::setprecision(1) << seconds
              << " s (not a benchmark: correctness run)\n";
    if (!r.parsed || !log.ok()) {
        std::cerr << (r.parsed ? "the log could not be written\n" : "the file is broken\n");
        return 1;
    }
    return 0;
}

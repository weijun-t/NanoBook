/*
 * nanobook_pipeline.cpp: replays an ITCH day through the three-thread pipeline into a binary log.
 *
 * What it does: Counts what each stock needs and sizes the books (phase 4), then replays the day
 *   through the pipeline (feed, engine and publisher threads; pipeline design §2), or with
 *   --sequential the same work on one thread, writing every TopOfBook update to a binary log
 *   (--log FILE; /dev/null if none). Prints the records sent, updates published, how often each
 *   ring was full, the wall time, and the log's size and checksum (--checksum). Exit code 0 if
 *   the whole day was read and the log written; 1 otherwise; 2 bad arguments; 3 no usable timer.
 *   Timing (step 4, pinned cores, pure spinning, the whole file loaded into memory first):
 *   --throughput runs the whole day on one thread then on three and reports messages per second;
 *   --latency replays unpaced up to --from (the warm-up that builds the books), then sends
 *   --rate records per second until --until, and reports the end-to-end latency percentiles
 *   from each record's intended send time to the publisher (pipeline design §9.2). Both also
 *   report the page faults taken during each run (optimise design §3.2).
 *     nanobook_pipeline FILE [--log OUT] [--checksum] [--sequential] [--cores A,B,C]
 *                       [--window-mb N]
 *     nanobook_pipeline FILE --cores A,B,C --throughput
 *     nanobook_pipeline FILE --cores A,B,C --latency --rate R [--from HH:MM:SS]
 *                       [--until HH:MM:SS] [--csv FILE.csv]
 *
 * Why it exists: Step 3 proves the pipeline changes nothing: the same day must give the same log,
 *   byte for byte, on three threads and on one (rule 11). Comparing checksums proves it without
 *   keeping two 20 GB files (WSL2's disk grows but never shrinks on its own). Step 4 measures
 *   throughput and the end-to-end latency (phase 5's exit criterion).
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
#include <cstdio>
#include <fstream>
#include <type_traits>
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
#include "environment.hpp"
#include "itch_timing.hpp"
#include "latency_recorder.hpp"
#include "mapped_file.hpp"
#include "pacing.hpp"
#include "page_faults.hpp"
#include "tsc.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/pipeline/stages.hpp"

namespace {

using namespace nanobook;
using namespace nanobook::pipeline;
using nanobook::latency::Environment;

constexpr std::string_view kUsage =
    "usage: nanobook_pipeline FILE [--log OUT] [--checksum] [--sequential] [--cores A,B,C]\n"
    "                         [--window-mb N]\n"
    "       nanobook_pipeline FILE --cores A,B,C --throughput\n"
    "       nanobook_pipeline FILE --cores A,B,C --latency --rate R [--from HH:MM:SS]\n"
    "                         [--until HH:MM:SS] [--csv FILE.csv]\n";

struct Options {
    std::string file;
    std::string log{"/dev/null"};
    bool checksum{false};
    bool sequential{false};
    PipelineOptions pipeline{};
    std::size_t window_mb{1024};
    // Timing (step 4, pipeline design §9)
    bool throughput{false};
    bool latency{false};
    std::uint64_t rate{0};                             // records per second, --latency
    std::uint64_t from{10ULL * 3'600 * 1'000'000'000}; // 10:00:00, ITCH time
    std::uint64_t until{10ULL * 3'600 * 1'000'000'000 + 60ULL * 1'000'000'000}; // 10:01:00
    std::string csv;
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
        if (arg == "--checksum" || arg == "--sequential") {
            (arg == "--checksum" ? o.checksum : o.sequential) = true;
            continue;
        }
        if (arg == "--throughput" || arg == "--latency") {
            (arg == "--throughput" ? o.throughput : o.latency) = true;
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
        } else if (arg == "--rate") {
            ok = parse_number(value, o.rate) && o.rate > 0;
        } else if (arg == "--from") {
            ok = parse_time(value, o.from);
        } else if (arg == "--until") {
            ok = parse_time(value, o.until);
        } else if (arg == "--csv") {
            o.csv = value;
            ok = !o.csv.empty();
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    if (o.file.empty() || (o.latency && o.rate == 0) ||
        ((o.throughput || o.latency) && o.pipeline.feed_core < 0)) {
        return std::nullopt; // timing needs pinned cores, and --latency a rate
    }
    o.pipeline.spin_only = o.throughput || o.latency; // pinned, dedicated cores: never yield
    return o;
}

// Passes messages on until the first one stamped after `until`, then drops the rest: the paced
// part of a latency run ends there (the rest of the file is parsed, quickly, but not sent).
template <typename Inner> class StopAfter {
public:
    StopAfter(Inner &inner, std::uint64_t until) : inner_(inner), until_(until) {
    }
    template <typename Message> void on(const Message &m) {
        stopped_ = stopped_ || m.timestamp > until_;
        if (!stopped_) {
            inner_.on(m);
        }
    }
    void on_skipped(char type) {
        if (!stopped_) {
            inner_.on_skipped(type);
        }
    }

private:
    Inner &inner_;
    std::uint64_t until_;
    bool stopped_{false};
};

// Counts the messages stamped in [from, until]: the most latency samples a run can take.
struct CountWindow {
    std::uint64_t from{};
    std::uint64_t until{};
    std::uint64_t count{};
    template <typename Message> void on(const Message &m) {
        count += m.timestamp >= from && m.timestamp <= until ? 1 : 0;
    }
    void on_skipped(char /*type*/) {
    }
};

// The time-stamp counter, as the pacer's clock.
struct TscClock {
    std::uint64_t operator()() const {
        return latency::start().ticks;
    }
};

using Builder = itch::BookBuilder<FastBook>;

std::string time_of_day(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000;
    std::array<char, 16> text{};
    std::snprintf(
        text.data(), text.size(), "%02llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600),
        static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
    return text.data();
}

void print_ns(double ns) {
    std::cout << std::setw(10) << std::fixed << std::setprecision(ns < 1'000 ? 1 : 0) << ns;
}

// Step 4 (pipeline design §9): throughput, or end-to-end latency at a paced rate. The whole file
// is loaded into memory first, untimed, so the feed never waits for the disk mid-run.
std::string faults_text(const latency::PageFaults &f) {
    return std::to_string(f.minor) + " minor, " + std::to_string(f.major) + " major";
}

int run_timing(const Options &o, std::span<const std::byte> bytes,
               const std::vector<BookConfig> &configs) {
    if (!latency::missing_tsc_flags(latency::read_cpu_flags()).empty()) {
        std::cerr << "the time-stamp counter can't be used as a clock here\n";
        return 3;
    }
    const Environment env = latency::collect_environment(o.pipeline.engine_core);
    const latency::Calibration calibration = latency::calibrate();
    const double rate = calibration.ticks_per_ns;
    const std::string banner = "*** NOT FOR PUBLISHING: " + env.unpublishable_reason() + " ***";
    if (!env.publishable()) {
        std::cout << banner << "\n";
    }
    for (const auto &[key, value] : env.lines()) {
        std::cout << std::left << std::setw(17) << key << value << "\n";
    }
    std::cout << std::setw(17) << "cores" << o.pipeline.feed_core << " (feed), "
              << o.pipeline.engine_core << " (engine), " << o.pipeline.publish_core
              << " (publisher), spinning only\n"
              << std::setw(17) << "counter" << std::fixed << std::setprecision(4) << rate
              << " ticks per ns\n"
              << std::right;
    itch::load_pages(bytes); // the whole file in memory before any clock starts

    const auto whole = [bytes](auto &handler) {
        const auto parsed = itch::parse(bytes, handler);
        return parsed.has_value() && *parsed == bytes.size();
    };

    if (o.throughput) {
        Builder one{configs, itch::BuilderOptions{}};
        BinaryLog log_one{"/dev/null"};
        const latency::PageFaults seq_f0 = latency::page_faults_now();
        const auto start = std::chrono::steady_clock::now();
        const PipelineResult seq = run_sequential(whole, one, log_one);
        const double seq_ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                .count();
        const latency::PageFaults seq_faults = latency::page_faults_now() - seq_f0;
        Builder three{configs, itch::BuilderOptions{}};
        BinaryLog log_three{"/dev/null"};
        const latency::PageFaults pipe_f0 = latency::page_faults_now();
        const PipelineResult pipe = run_pipeline(whole, three, log_three, o.pipeline);
        // Read after the threads have ended, so it includes run_pipeline's start-up: the two
        // rings' slots are zero-filled there (7 MB, about 1,800 pages) and the thread stacks.
        const latency::PageFaults pipe_faults = latency::page_faults_now() - pipe_f0;
        const auto messages = static_cast<double>(seq.records + seq.skipped);
        std::cout << "\nThroughput: the whole day, " << seq.records + seq.skipped
                  << " messages, logs to /dev/null\n"
                  << std::setprecision(2) << "  one thread:     " << seq_ns / 1e9 << " s, "
                  << std::setprecision(1) << seq_ns / messages << " ns per message, "
                  << messages / seq_ns * 1e3 << " M messages/s\n"
                  << std::setprecision(2) << "  three threads:  " << pipe.elapsed_ns / 1e9 << " s, "
                  << std::setprecision(1) << pipe.elapsed_ns / messages << " ns per message, "
                  << messages / pipe.elapsed_ns * 1e3 << " M messages/s (" << std::setprecision(2)
                  << seq_ns / pipe.elapsed_ns << "x)\n"
                  << "  ring 1 full " << pipe.feed_waits << " tries, ring 2 full "
                  << pipe.engine_waits << " tries; "
                  << (pipe.feed_pinned && pipe.engine_pinned && pipe.publish_pinned
                          ? "all three pinned"
                          : "NOT all pinned")
                  << "\n  page faults during each run: one thread " << faults_text(seq_faults)
                  << "; three threads " << faults_text(pipe_faults)
                  << " (start-up included: building the rings and threads)\n";
        if (!seq.parsed || !pipe.parsed || seq.updates != pipe.updates) {
            std::cerr << "the two runs disagree, or the file is broken\n";
            return 1;
        }
    }

    if (o.latency) {
        CountWindow window{.from = o.from, .until = o.until};
        if (!itch::parse(bytes, window)) {
            std::cerr << "the file is broken\n";
            return 1;
        }
        latency::Recorder recorder{window.count + 1'000};
        Builder builder{configs, itch::BuilderOptions{}};
        BinaryLog log{"/dev/null"};
        auto sink = [&](const TopOfBook &t) {
            log.append(t);
            if (t.sent != 0) { // 0: the warm-up, not paced
                recorder.record_across_cores(latency::stop().ticks - t.sent, latency::Tag::Other);
            }
        };
        const auto ticks_per_second = static_cast<std::uint64_t>(rate * 1e9);
        Pacer pacer{TscClock{}, ticks_per_second, o.rate, o.from, ticks_per_second / 10}; // 100 ms
        auto stamp = [&pacer](const FeedRecord &r) { return pacer(r); };
        const auto until = [&](auto &handler) {
            StopAfter<std::remove_reference_t<decltype(handler)>> stop{handler, o.until};
            return itch::parse(bytes, stop).has_value();
        };
        const latency::PageFaults f0 = latency::page_faults_now();
        const PipelineResult r = run_pipeline(until, builder, sink, o.pipeline, stamp);
        const latency::PageFaults faults = latency::page_faults_now() - f0;
        const latency::Summary s = latency::summarize(recorder, 0, rate, std::nullopt);
        std::cout << "\nLatency: " << o.rate / 1'000 << " K records/s paced from "
                  << time_of_day(o.from) << " to " << time_of_day(o.until) << " (" << pacer.paced()
                  << " records paced, " << s.count
                  << " updates timed; ns, from each record's intended send time to the "
                     "publisher)\n"
                  << "       min       p50       p90       p99     p99.9    p99.99       max"
                     "      mean\n";
        for (const double v : {s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns, s.p999_ns}) {
            print_ns(v);
        }
        if (s.count >= 1'000'000) { // p99.99 rests on at least 100 samples (phase 3's rule)
            print_ns(s.p9999_ns);
        } else {
            std::cout << std::setw(10) << "-";
        }
        print_ns(s.max_ns);
        print_ns(s.mean_ns);
        std::cout << "\n  clamped (over 1.7 s) " << recorder.clamped() << ", refused (full) "
                  << recorder.refused() << "; ring 1 full " << r.feed_waits
                  << " tries, ring 2 full " << r.engine_waits << " tries\n"
                  << "  page faults during the run (start-up and warm-up included): "
                  << faults_text(faults) << "\n";
        if (!o.csv.empty()) {
            std::ofstream f{o.csv};
            if (!env.publishable()) {
                f << "# " << banner << "\n";
            }
            for (const auto &[key, value] : env.lines()) {
                f << "# " << key << ": " << value << "\n";
            }
            f << "# cores: " << o.pipeline.feed_core << "," << o.pipeline.engine_core << ","
              << o.pipeline.publish_core << "\n# input: " << o.file << "\n# paced: " << o.rate
              << " records/s from " << time_of_day(o.from) << " to " << time_of_day(o.until) << "\n"
              << "mode,scenario,book,tag,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,"
                 "max_ns,mean_ns,dropped,clamped,timed_allocations\n"
              << std::fixed << std::setprecision(1) << "latency,pipeline_" << o.rate
              << ",pipeline,all," << s.count << "," << s.min_ns << "," << s.p50_ns << ","
              << s.p90_ns << "," << s.p99_ns << "," << s.p999_ns << ",";
            if (s.count >= 1'000'000) {
                f << s.p9999_ns;
            }
            f << "," << s.max_ns << "," << s.mean_ns << ",0," << recorder.clamped() << ",\n";
            std::cout << "wrote " << o.csv << "\n";
        }
    }
    if (!env.publishable()) {
        std::cout << banner << "\n";
    }
    return 0;
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
    const std::vector<BookConfig> configs =
        itch::size_books(counter.directory(), counter.stats(), itch::SizingRules{});
    if (o.throughput || o.latency) {
        return run_timing(o, bytes, configs);
    }
    itch::BookBuilder<FastBook> builder{configs, itch::BuilderOptions{}};
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

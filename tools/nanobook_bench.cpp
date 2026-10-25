/*
 * nanobook_bench.cpp: the latency harness's command line (phase 3).
 *
 * What it does: Times every operation of each scenario (support/latency_scenarios.hpp) on each
 *   book, one by one with the CPU's time-stamp counter, and prints the distribution per kind of
 *   operation: count, min, p50, p90, p99, p99.9, p99.99, max and mean, in nanoseconds, under the
 *   environment it was measured in. Optionally writes CSV files for the plot script.
 *     nanobook_bench [--scenario NAME|all] [--book reference|sorted|tick|all] [--ops N]
 *                    [--seed S] [--core C] [--throughput] [--csv FILE.csv] [--raw FILE.csv]
 *   --csv writes the summary to FILE.csv and the histogram buckets to FILE_histogram.csv.
 *   --raw writes every sample, in the order it was taken (large: one line per operation).
 *   --throughput times whole batches instead of each operation (design §6.3).
 *   Exit code 0 means every run finished, 2 bad arguments or an unwritable file, 3 the counter or
 *   the pinning is not usable on this machine.
 *
 * Why it exists: Google Benchmark reports averages; phase 3 reports the tail too (plan rules 16,
 *   17). Every published latency number comes from this tool, with its environment (rule 19).
 *
 * How it works: First the environment (support/environment.hpp) and the timer: check the
 *   counter's CPU flags, pin to --core C if given, calibrate ticks per ns, measure the timer's own
 *   cost (its minimum is subtracted from every sample, design §13 Q2). In a virtual machine, or
 *   built from uncommitted changes, every output carries a NOT FOR PUBLISHING banner with the
 *   reason (rules 19, 20). Then, for each scenario and book: a Recorder with room for N samples,
 *   run_scenario(), summarize() per tag. The allocations made while timed operations ran are
 *   counted with the counting operator new (the fast books must show 0, rule 1). Last, the counter
 *   is calibrated again to show any drift; the CSV files are written at the end, so they include
 *   it.
 *
 * Trade-offs: A tool, not the engine: it prints with iostream, writes files and calls the system
 *   clock, which the hot path must never do (rule 6). All of that happens outside timed regions.
 *   Linking the counting operator new adds one atomic increment per allocation; none happen while
 *   timing a fast book.
 */

#include <sched.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alloc_counter.hpp"
#include "environment.hpp"
#include "latency_recorder.hpp"
#include "latency_scenarios.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/reference/reference_book.hpp"
#include "tsc.hpp"

namespace {

using namespace nanobook;
using namespace nanobook::latency;

constexpr std::string_view kUsage =
    "usage: nanobook_bench [--scenario NAME|all] [--book reference|sorted|tick|all] [--ops N]\n"
    "                      [--seed S] [--core C] [--throughput] [--csv FILE.csv] [--raw FILE.csv]\n"
    "scenarios: add cancel modify match sweep10 large_cancel large_add deep_insert mixed\n";

// Only printed when !env.publishable().
std::string banner(const Environment &env) {
    return "*** NOT FOR PUBLISHING: " + env.unpublishable_reason() + " ***";
}

enum class BookKind : std::uint8_t { Reference, Sorted, Tick };
constexpr std::array<BookKind, 3> kAllBooks = {BookKind::Reference, BookKind::Sorted,
                                               BookKind::Tick};

constexpr std::string_view book_name(BookKind b) {
    switch (b) {
    case BookKind::Reference:
        return "ReferenceBook";
    case BookKind::Sorted:
        return "FastBook";
    case BookKind::Tick:
        return "TickFastBook";
    }
    return "?";
}

// Reads a whole decimal number; false if `text` isn't one.
template <typename T> bool parse(std::string_view text, T &value) {
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

// Moves this program onto `core` and checks that it is running there.
bool pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set); // core >= 0: checked by the caller
    return sched_setaffinity(0, sizeof(set), &set) == 0 && sched_getcpu() == core;
}

// "out.csv" -> "out_histogram.csv"; "out" -> "out_histogram.csv".
std::string histogram_path(std::string_view csv) {
    const std::string_view stem = csv.ends_with(".csv") ? csv.substr(0, csv.size() - 4) : csv;
    return std::string{stem} + "_histogram.csv";
}

struct Timer {
    Calibration calibration;
    Overhead overhead;
};

ScenarioResult run(BookKind b, Scenario s, Recorder &recorder, const Options &o) {
    switch (b) {
    case BookKind::Reference:
        return run_scenario<ReferenceBook>(s, recorder, o);
    case BookKind::Sorted:
        return run_scenario<FastBook>(s, recorder, o);
    case BookKind::Tick:
        return run_scenario<TickFastBook>(s, recorder, o);
    }
    return {};
}

// ---------------------------------------------------------------------------
// What one run produced, kept for the CSV files written at the end
// ---------------------------------------------------------------------------

struct Row {
    std::string_view mode; // "latency" or "throughput"
    Scenario scenario{};
    BookKind book{};
    std::string_view tag; // a tag name, or "all"
    Summary summary;      // throughput mode: only count and mean_ns
    std::uint64_t dropped{0};
    std::uint64_t clamped{0};
    std::uint64_t timed_allocations{0};
};

struct HistogramRow {
    Scenario scenario{};
    BookKind book{};
    std::string_view tag;
    Bucket bucket;
};

// ---------------------------------------------------------------------------
// Terminal output
// ---------------------------------------------------------------------------

// A time in ns: one decimal below 1,000 ns, whole numbers above.
void print_ns(double ns) {
    std::cout << std::setw(10) << std::fixed << std::setprecision(ns < 1'000 ? 1 : 0) << ns;
}

void print_row(std::string_view label, const Summary &s) {
    std::cout << std::left << std::setw(8) << label << std::right << std::setw(10) << s.count;
    for (const double v : {s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns, s.p999_ns}) {
        print_ns(v);
    }
    if (s.count >= 1'000'000) { // p99.99 rests on at least 100 samples (design §5.2)
        print_ns(s.p9999_ns);
    } else {
        std::cout << std::setw(10) << "-";
    }
    print_ns(s.max_ns);
    print_ns(s.mean_ns);
    std::cout << "\n";
}

void print_environment(const Environment &env, const Timer &timer, const Options &o, int core) {
    if (!env.publishable()) {
        std::cout << banner(env) << "\n";
    }
    for (const auto &[key, value] : env.lines()) {
        std::cout << std::left << std::setw(17) << key << value << "\n";
    }
    const double rate = timer.calibration.ticks_per_ns;
    std::cout << std::setw(17) << "pinned to core" << (core >= 0 ? std::to_string(core) : "no")
              << "\n"
              << std::setw(17) << "counter" << std::fixed << std::setprecision(4) << rate
              << " ticks per ns (spread " << std::setprecision(3) << timer.calibration.spread * 1e6
              << " parts per million)\n"
              << std::setw(17) << "timer overhead" << "min " << timer.overhead.min
              << " ticks = " << std::setprecision(1) << to_ns(timer.overhead.min, rate)
              << " ns (subtracted from every sample), median " << to_ns(timer.overhead.median, rate)
              << " ns\n"
              << std::setw(17) << "mode" << (o.throughput ? "throughput" : "latency") << ", "
              << o.ops << (o.throughput ? " operations" : " samples") << " per run, seed " << o.seed
              << "\n"
              << std::right;
}

// ---------------------------------------------------------------------------
// CSV files
// ---------------------------------------------------------------------------

// The environment and timer as "# key: value" comment lines at the top of every CSV file.
void write_csv_header(std::ostream &f, const Environment &env, const Timer &timer, const Options &o,
                      int core, std::optional<double> drift) {
    if (!env.publishable()) {
        f << "# " << banner(env) << "\n";
    }
    for (const auto &[key, value] : env.lines()) {
        f << "# " << key << ": " << value << "\n";
    }
    f << std::fixed << "# pinned to core: " << (core >= 0 ? std::to_string(core) : "no") << "\n"
      << std::setprecision(6) << "# counter ticks per ns: " << timer.calibration.ticks_per_ns
      << "\n"
      << "# counter spread (parts per million): " << timer.calibration.spread * 1e6 << "\n";
    if (drift) {
        f << "# counter drift during the run (parts per million): " << *drift * 1e6 << "\n";
    }
    f << "# timer overhead min (ticks, subtracted): " << timer.overhead.min << "\n"
      << "# timer overhead median (ticks): " << timer.overhead.median << "\n"
      << "# mode: " << (o.throughput ? "throughput" : "latency") << "\n"
      << "# per run: " << o.ops << "\n"
      << "# seed: " << o.seed << "\n";
}

void write_summary_csv(std::ostream &f, const std::vector<Row> &rows) {
    f << "mode,scenario,book,tag,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,"
         "mean_ns,dropped,clamped,timed_allocations\n";
    f << std::fixed << std::setprecision(1);
    for (const Row &r : rows) {
        const Summary &s = r.summary;
        f << r.mode << "," << scenario_name(r.scenario) << "," << book_name(r.book) << "," << r.tag
          << "," << s.count << ",";
        if (r.mode == "latency") {
            f << s.min_ns << "," << s.p50_ns << "," << s.p90_ns << "," << s.p99_ns << ","
              << s.p999_ns << ",";
            if (s.count >= 1'000'000) {
                f << s.p9999_ns;
            }
            f << "," << s.max_ns << ",";
        } else {
            f << ",,,,,,,"; // throughput mode measures only the mean
        }
        f << s.mean_ns << "," << r.dropped << "," << r.clamped << "," << r.timed_allocations
          << "\n";
    }
}

void write_histogram_csv(std::ostream &f, const std::vector<HistogramRow> &rows) {
    f << "scenario,book,tag,low_ns,high_ns,count\n";
    for (const HistogramRow &r : rows) {
        f << scenario_name(r.scenario) << "," << book_name(r.book) << "," << r.tag << ","
          << r.bucket.low_ns << "," << r.bucket.high_ns << "," << r.bucket.count << "\n";
    }
}

// Every sample of one run, in the order it was taken (overhead subtracted, whole ns).
void write_raw(std::ostream &f, Scenario s, BookKind b, const Recorder &recorder,
               const Timer &timer) {
    const auto ticks = recorder.ticks();
    const auto tags = recorder.tags();
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const auto ns = static_cast<std::uint64_t>(
            to_ns(minus_overhead(ticks[i], timer.overhead.min), timer.calibration.ticks_per_ns));
        f << scenario_name(s) << "," << book_name(b) << "," << i << "," << tag_name(tags[i]) << ","
          << ns << "\n";
    }
}

} // namespace

int main(int argc, char **argv) {
    std::optional<Scenario> only_scenario;
    std::optional<BookKind> only_book;
    Options options;
    int core = -1;
    std::string csv_path;
    std::string raw_path;

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--throughput") { // the only flag without a value
            options.throughput = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << flag << "\n" << kUsage;
            return 2;
        }
        const std::string_view value = argv[++i];
        bool ok = false;
        if (flag == "--scenario") {
            only_scenario = parse_scenario(value);
            ok = value == "all" || only_scenario.has_value();
        } else if (flag == "--book") {
            ok = true;
            if (value == "reference") {
                only_book = BookKind::Reference;
            } else if (value == "sorted") {
                only_book = BookKind::Sorted;
            } else if (value == "tick") {
                only_book = BookKind::Tick;
            } else {
                ok = value == "all";
            }
        } else if (flag == "--ops") {
            ok = parse(value, options.ops) && options.ops > 0;
        } else if (flag == "--seed") {
            ok = parse(value, options.seed);
        } else if (flag == "--core") {
            ok = parse(value, core) && core >= 0;
        } else if (flag == "--csv") {
            csv_path = value;
            ok = !csv_path.empty();
        } else if (flag == "--raw") {
            raw_path = value;
            ok = !raw_path.empty();
        }
        if (!ok) {
            std::cerr << "bad argument: " << flag << " " << value << "\n" << kUsage;
            return 2;
        }
    }
    if (options.throughput && !raw_path.empty()) {
        std::cerr << "--raw needs per-operation samples: it can't be combined with --throughput\n";
        return 2;
    }

    std::cout << "nanobook_bench: per-operation latency (docs/latency-harness-design.md)\n";

    const auto missing = missing_tsc_flags(read_cpu_flags());
    if (!missing.empty()) {
        std::cerr << "the time-stamp counter can't be used as a clock here; missing CPU flags:";
        for (const std::string_view flag : missing) {
            std::cerr << " " << flag;
        }
        std::cerr << "\n";
        return 3;
    }
    if (core >= 0 && !pin_to_core(core)) {
        std::cerr << "could not pin to core " << core << "\n";
        return 3;
    }

    const Environment env = collect_environment(core);
    const Timer timer{.calibration = calibrate(), .overhead = measure_overhead()};
    const double rate = timer.calibration.ticks_per_ns;
    print_environment(env, timer, options, core);

    std::ofstream raw;
    if (!raw_path.empty()) {
        raw.open(raw_path);
        if (!raw) {
            std::cerr << "can't write " << raw_path << "\n";
            return 2;
        }
        write_csv_header(raw, env, timer, options, core, std::nullopt);
        raw << "scenario,book,index,tag,ns\n";
    }

    options.allocations = &support::allocations;
    Recorder recorder{options.throughput ? 1 : options.ops}; // throughput mode records nothing
    std::vector<Row> rows;
    std::vector<HistogramRow> histogram_rows;

    for (const Scenario s : kAllScenarios) {
        if (only_scenario && *only_scenario != s) {
            continue;
        }
        for (const BookKind b : kAllBooks) {
            if (only_book && *only_book != b) {
                continue;
            }
            recorder.clear();
            const ScenarioResult result = run(b, s, recorder, options);

            if (options.throughput) {
                const double mean = to_ns(result.throughput_ticks, rate) /
                                    static_cast<double>(result.throughput_ops);
                std::cout << "\n== " << scenario_name(s) << "  " << book_name(b)
                          << "  throughput: " << result.throughput_ops << " operations, mean "
                          << std::fixed << std::setprecision(1) << mean << " ns, "
                          << std::setprecision(2) << 1e3 / mean << " M ops/s, "
                          << result.timed_allocations << " allocations while timing\n";
                rows.push_back(
                    Row{.mode = "throughput",
                        .scenario = s,
                        .book = b,
                        .tag = "all",
                        .summary = Summary{.count = result.throughput_ops, .mean_ns = mean},
                        .timed_allocations = result.timed_allocations});
                continue;
            }

            std::cout << "\n== " << scenario_name(s) << "  " << book_name(b) << "  "
                      << recorder.size() << " samples  " << result.timed_allocations
                      << " allocations while timing\n"
                      << "tag          count       min       p50       p90       p99     p99.9"
                         "    p99.99       max      mean  (ns)\n";
            // One row per tag seen, plus "all" when there is more than one.
            std::vector<std::pair<std::string_view, std::optional<Tag>>> groups;
            for (const Tag tag : kAllTags) {
                if (std::ranges::find(recorder.tags(), tag) != recorder.tags().end()) {
                    groups.emplace_back(tag_name(tag), tag);
                }
            }
            if (groups.size() > 1) {
                groups.emplace_back("all", std::nullopt);
            }
            for (const auto &[label, tag] : groups) {
                const Summary sum = summarize(recorder, timer.overhead.min, rate, tag);
                print_row(label, sum);
                rows.push_back(Row{.mode = "latency",
                                   .scenario = s,
                                   .book = b,
                                   .tag = label,
                                   .summary = sum,
                                   .dropped = recorder.dropped(),
                                   .clamped = recorder.clamped(),
                                   .timed_allocations = result.timed_allocations});
                if (!csv_path.empty()) {
                    const auto sorted = sorted_samples(recorder, timer.overhead.min, tag);
                    for (const Bucket &bucket : histogram(sorted, rate)) {
                        histogram_rows.push_back(
                            HistogramRow{.scenario = s, .book = b, .tag = label, .bucket = bucket});
                    }
                }
            }
            if (recorder.dropped() + recorder.clamped() + recorder.refused() > 0) {
                std::cout << "  dropped (moved core) " << recorder.dropped()
                          << ", clamped (too long) " << recorder.clamped() << ", refused (full) "
                          << recorder.refused() << "\n";
            }
            if (raw.is_open()) {
                write_raw(raw, s, b, recorder, timer);
            }
        }
    }

    const Calibration after = calibrate();
    const double drift = (after.ticks_per_ns - rate) / rate;
    std::cout << "\ncounter at the end: " << std::setprecision(4) << after.ticks_per_ns
              << " ticks per ns (drift " << std::setprecision(3) << std::abs(drift) * 1e6
              << " parts per million)\n";

    if (!csv_path.empty()) {
        std::ofstream summary{csv_path};
        std::ofstream hist{histogram_path(csv_path)};
        if (!summary || !hist) {
            std::cerr << "can't write " << csv_path << " or " << histogram_path(csv_path) << "\n";
            return 2;
        }
        write_csv_header(summary, env, timer, options, core, drift);
        write_summary_csv(summary, rows);
        write_csv_header(hist, env, timer, options, core, drift);
        write_histogram_csv(hist, histogram_rows);
        std::cout << "wrote " << csv_path << " and " << histogram_path(csv_path) << "\n";
    }
    if (!env.publishable()) {
        std::cout << banner(env) << "\n";
    }
    return 0;
}

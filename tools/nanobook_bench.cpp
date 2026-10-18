/*
 * nanobook_bench.cpp: the latency harness's command line (phase 3).
 *
 * What it does: Times every operation of each scenario (support/latency_scenarios.hpp) on each
 *   book, one by one with the CPU's time-stamp counter, and prints the distribution per kind of
 *   operation: count, min, p50, p90, p99, p99.9, p99.99, max and mean, in nanoseconds.
 *     nanobook_bench [--scenario NAME|all] [--book reference|sorted|tick|all] [--ops N]
 *                    [--seed S] [--core C]
 *   Exit code 0 means every run finished, 2 bad arguments, 3 the counter or the pinning is not
 *   usable on this machine.
 *
 * Why it exists: Google Benchmark reports averages; phase 3 reports the tail too (README rules 16,
 *   17). Every published latency number comes from this tool.
 *
 * How it works: First the timer: check the counter's CPU flags, pin to --core C if given,
 *   calibrate ticks per ns, measure the timer's own cost (its minimum is subtracted from every
 *   sample, latency-harness design §13 Q2). Then, for each scenario and book: a Recorder with
 *   room for N samples, run_scenario(), summarize() per tag. The allocations made while timed
 *   operations ran are counted with the counting operator new and printed (the fast books must
 *   show 0, rule 1). Last, the counter is calibrated again to show any drift.
 *
 * Trade-offs: A tool, not the engine: it prints with iostream and calls the system clock, which
 *   the hot path must never do (rule 6). All printing happens outside timed regions. Linking the
 *   counting operator new adds one atomic increment per allocation; none happen while timing a
 *   fast book.
 */

#include <sched.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

#include "alloc_counter.hpp"
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
    "                      [--seed S] [--core C]\n"
    "scenarios: add cancel modify match sweep10 large_cancel large_add deep_insert mixed\n";

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

void print_result(BookKind b, Scenario s, const Recorder &recorder, const ScenarioResult &result,
                  const Timer &timer) {
    std::cout << "\n== " << scenario_name(s) << "  " << book_name(b) << "  " << recorder.size()
              << " samples  " << result.timed_allocations << " allocations while timing\n";
    std::cout << "tag          count       min       p50       p90       p99     p99.9    p99.99"
                 "       max      mean  (ns)\n";
    const std::uint64_t overhead = timer.overhead.min;
    const double rate = timer.calibration.ticks_per_ns;
    int tags_seen = 0;
    for (const Tag tag : kAllTags) {
        const Summary sum = summarize(recorder, overhead, rate, tag);
        if (sum.count > 0) {
            print_row(tag_name(tag), sum);
            ++tags_seen;
        }
    }
    if (tags_seen > 1) {
        print_row("all", summarize(recorder, overhead, rate));
    }
    if (recorder.dropped() + recorder.clamped() + recorder.refused() > 0) {
        std::cout << "  dropped (moved core) " << recorder.dropped() << ", clamped (too long) "
                  << recorder.clamped() << ", refused (full) " << recorder.refused() << "\n";
    }
}

} // namespace

int main(int argc, char **argv) {
    std::optional<Scenario> only_scenario;
    std::optional<BookKind> only_book;
    Options options;
    int core = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
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
        }
        if (!ok) {
            std::cerr << "bad argument: " << flag << " " << value << "\n" << kUsage;
            return 2;
        }
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
    if (core >= 0) {
        if (!pin_to_core(core)) {
            std::cerr << "could not pin to core " << core << "\n";
            return 3;
        }
        std::cout << "pinned to core " << core << "\n";
    } else {
        std::cout << "not pinned (use --core C for real measurements)\n";
    }

    const Timer timer{.calibration = calibrate(), .overhead = measure_overhead()};
    const double rate = timer.calibration.ticks_per_ns;
    std::cout << "counter: " << std::fixed << std::setprecision(4) << rate
              << " ticks per ns (spread " << std::setprecision(3) << timer.calibration.spread * 1e6
              << " parts per million)\n"
              << "timer overhead: min " << timer.overhead.min << " ticks = " << std::setprecision(1)
              << to_ns(timer.overhead.min, rate) << " ns (subtracted from every sample), median "
              << to_ns(timer.overhead.median, rate) << " ns\n"
              << "samples per run: " << options.ops << ", seed " << options.seed << "\n";

    options.allocations = &support::allocations;
    Recorder recorder{options.ops};
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
            print_result(b, s, recorder, result, timer);
        }
    }

    const Calibration after = calibrate();
    const double drift = (after.ticks_per_ns - rate) / rate;
    std::cout << "\ncounter at the end: " << std::setprecision(4) << after.ticks_per_ns
              << " ticks per ns (drift " << std::setprecision(3) << std::abs(drift) * 1e6
              << " parts per million)\n";
    return 0;
}

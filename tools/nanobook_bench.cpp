/*
 * nanobook_bench.cpp: the latency harness's command line (phase 3).
 *
 * What it does: For now (phase 3 step 1) it checks the timer: that the CPU's time-stamp counter is
 *   safe to use as a clock, how many ticks make a nanosecond (and how stable that is), and what an
 *   empty start/stop pair costs. Scenarios and per-operation results arrive in step 3.
 *     nanobook_bench [--core C]
 *   Exit code 0 means the timer is usable, 2 bad arguments, 3 the counter or the pinning is not
 *   usable on this machine.
 *
 * Why it exists: Every latency number phase 3 publishes rests on this timer, so its rate and its
 *   own cost are printed first (latency-harness design §3.4, §4).
 *
 * How it works: --core C pins the program to core C (sched_setaffinity) before anything is
 *   measured, and checks it really runs there. Then calibrate() and measure_overhead() from
 *   support/tsc.hpp. The tick rate is measured again at the end, to show any drift.
 *
 * Trade-offs: A tool, not the engine: it prints with iostream and calls the system clock, which
 *   the hot path must never do (rule 6). All printing happens outside timed regions.
 */

#include <sched.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

#include "tsc.hpp"

namespace {

using namespace nanobook::latency;

constexpr std::string_view kUsage = "usage: nanobook_bench [--core C]\n";

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

void print_ticks(std::string_view label, std::uint64_t ticks, double ticks_per_ns) {
    std::cout << "  " << label << " " << ticks << " ticks = " << std::fixed << std::setprecision(1)
              << to_ns(ticks, ticks_per_ns) << " ns\n";
}

} // namespace

int main(int argc, char **argv) {
    int core = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << flag << "\n" << kUsage;
            return 2;
        }
        const std::string_view value = argv[++i];
        const bool ok = flag == "--core" && parse(value, core) && core >= 0;
        if (!ok) {
            std::cerr << "bad argument: " << flag << " " << value << "\n" << kUsage;
            return 2;
        }
    }

    std::cout << "nanobook_bench: timer check (phase 3 step 1)\n";

    const auto missing = missing_tsc_flags(read_cpu_flags());
    if (!missing.empty()) {
        std::cerr << "the time-stamp counter can't be used as a clock here; missing CPU flags:";
        for (const std::string_view flag : missing) {
            std::cerr << " " << flag;
        }
        std::cerr << "\n";
        return 3;
    }
    std::cout << "counter flags: constant_tsc nonstop_tsc rdtscp all present\n";

    if (core >= 0) {
        if (!pin_to_core(core)) {
            std::cerr << "could not pin to core " << core << "\n";
            return 3;
        }
        std::cout << "pinned to core " << core << "\n";
    } else {
        std::cout << "not pinned (use --core C for real measurements)\n";
    }

    const Calibration before = calibrate();
    std::cout << "counter rate: " << std::fixed << std::setprecision(4) << before.ticks_per_ns
              << " ticks per ns (5 rounds of 200 ms, spread " << std::setprecision(3)
              << before.spread * 1e6 << " parts per million)\n";

    constexpr std::size_t kSamples = 1'000'000;
    const Overhead overhead = measure_overhead(kSamples);
    std::cout << "timer overhead, " << kSamples << " empty start/stop pairs:\n";
    print_ticks("min   ", overhead.min, before.ticks_per_ns);
    print_ticks("median", overhead.median, before.ticks_per_ns);
    print_ticks("max   ", overhead.max, before.ticks_per_ns);

    const Calibration after = calibrate();
    const double drift = (after.ticks_per_ns - before.ticks_per_ns) / before.ticks_per_ns;
    std::cout << "counter rate at the end: " << std::setprecision(4) << after.ticks_per_ns
              << " ticks per ns (drift " << std::setprecision(3) << std::abs(drift) * 1e6
              << " parts per million)\n";
    return 0;
}

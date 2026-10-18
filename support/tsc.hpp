/*
 * tsc.hpp: a nanosecond timer built on the CPU's time-stamp counter (TSC).
 *
 * What it does: start() and stop() read the CPU's time-stamp counter around one operation, and
 *   report which core each reading came from. calibrate() finds how many counter ticks make a
 *   nanosecond, measure_overhead() finds what an empty start()/stop() pair costs, and
 *   missing_tsc_flags() checks that this CPU's counter is safe to use as a clock.
 *
 * Why it exists: Phase 3 times every single operation (latency-harness design §3). A system clock
 *   call (clock_gettime) does too much work to wrap a 10 ns operation and is banned in tight loops
 *   (rule 6). The counter is read by one instruction, with no system call.
 *
 * How it works: Every x86 CPU has a 64-bit counter that rises at a fixed rate from power-on.
 *   - start()/stop(): `rdtscp` waits until every earlier instruction has finished, then reads the
 *     counter; `lfence` after it stops later instructions from starting before the read. So the
 *     timed operation can't leak out of the two readings in either direction (design §3.3).
 *     `rdtscp` also returns a value Linux sets to the core number, so a sample taken across a
 *     move to another core can be spotted and dropped.
 *   - A compiler barrier (`asm volatile("" ::: "memory")`) on both sides of each reading stops
 *     the compiler from moving the operation's memory reads and writes across it.
 *   - calibrate(): reads the counter and CLOCK_MONOTONIC_RAW together, waits, reads both again;
 *     ticks per ns = counter difference / clock difference. A reading pair is retried if the
 *     thread was paused between the two reads. 5 rounds, the median is used, and the spread
 *     between rounds is reported (design §4). Cold path: the system calls are fine.
 *   - measure_overhead(): times an empty region n times and sorts the results: O(n log n).
 *
 * Data structures: Stamp is 16 bytes: the counter value and the core it was read on.
 *   PairedReading is a counter value and the system-clock time it was read at.
 *
 * Trade-offs: x86-64 only (rdtscp and lfence are x86 instructions). Ticks are not CPU cycles: the
 *   counter ticks at a fixed rate while the real clock changes with turbo, so results are
 *   reported in nanoseconds. Tool and test code only: it allocates and reads files outside the
 *   timed region.
 */

#pragma once

#if !defined(__x86_64__)
#error "tsc.hpp needs x86-64: it uses the rdtscp and lfence instructions"
#endif

#include <x86intrin.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace nanobook::latency {

// ---------------------------------------------------------------------------
// Reading the counter
// ---------------------------------------------------------------------------

struct Stamp {
    std::uint64_t ticks{0};
    std::uint32_t core{0}; // the value Linux puts in TSC_AUX: (NUMA node << 12) | core number
};

// Tells the compiler that any memory may have changed here, so it keeps every memory read and
// write on its own side. Emits no instruction.
inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

// One reading: wait for earlier instructions, read, block later instructions until the read is
// done. Always inlined, so no function call is added to the timed region.
[[gnu::always_inline]] inline Stamp read_stamp() {
    compiler_barrier();
    unsigned int aux = 0;
    const std::uint64_t ticks = __rdtscp(&aux);
    _mm_lfence();
    compiler_barrier();
    return Stamp{.ticks = ticks, .core = aux};
}

// start() and stop() are the same reading; the two names make the timed region easy to see:
//   const Stamp t0 = start();  work();  const Stamp t1 = stop();
[[gnu::always_inline]] inline Stamp start() {
    return read_stamp();
}
[[gnu::always_inline]] inline Stamp stop() {
    return read_stamp();
}

// ---------------------------------------------------------------------------
// Is the counter safe to use as a clock?
// ---------------------------------------------------------------------------

// The CPU flags (/proc/cpuinfo) the harness needs: a counter that ticks at a fixed rate
// (constant_tsc), keeps ticking in power-saving states (nonstop_tsc), and the rdtscp instruction.
inline constexpr std::string_view kRequiredFlags[] = {"constant_tsc", "nonstop_tsc", "rdtscp"};

// True if `flag` is one of the space-separated words in `flags`.
inline bool has_flag(std::string_view flags, std::string_view flag) {
    std::size_t pos = 0;
    while (pos < flags.size()) {
        const std::size_t end = std::min(flags.find(' ', pos), flags.size());
        if (flags.substr(pos, end - pos) == flag) {
            return true;
        }
        pos = end + 1;
    }
    return false;
}

// The required flags that `flags` doesn't contain. Empty means the counter is safe to use.
inline std::vector<std::string_view> missing_tsc_flags(std::string_view flags) {
    std::vector<std::string_view> missing;
    for (const std::string_view flag : kRequiredFlags) {
        if (!has_flag(flags, flag)) {
            missing.push_back(flag);
        }
    }
    return missing;
}

// The first "flags : ..." line of /proc/cpuinfo, without the "flags : " part. Empty if the file
// can't be read.
inline std::string read_cpu_flags() {
    std::ifstream cpuinfo{"/proc/cpuinfo"};
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("flags")) {
            const std::size_t colon = line.find(':');
            return colon == std::string::npos ? std::string{} : line.substr(colon + 1);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Ticks to nanoseconds
// ---------------------------------------------------------------------------

// The system clock in nanoseconds. CLOCK_MONOTONIC_RAW is never adjusted by network time sync,
// so it can't be sped up or slowed down while calibrating. Cold path only (a clock call).
inline std::int64_t monotonic_raw_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return std::int64_t{ts.tv_sec} * 1'000'000'000 + std::int64_t{ts.tv_nsec};
}

inline double to_ns(std::uint64_t ticks, double ticks_per_ns) {
    return static_cast<double>(ticks) / ticks_per_ns;
}

struct Calibration {
    double ticks_per_ns{0}; // the median of the rounds
    double spread{0};       // (highest - lowest) / median, e.g. 0.0001 = 0.01%
};

// The counter and the system clock read at (almost) the same moment. The clock is read on both
// sides of the counter, and the time used is the middle of the two. If the thread was paused in
// between (a gap over 2 µs), a pause there would make the whole round wrong, so it tries again:
// up to 1,000 times, keeping the pair with the smallest gap.
struct PairedReading {
    std::uint64_t ticks{0};
    double ns{0};
};

inline PairedReading paired_reading() {
    constexpr std::int64_t kGoodGapNs = 2'000;
    constexpr int kTries = 1'000;
    PairedReading best{};
    std::int64_t best_gap = -1;
    for (int i = 0; i < kTries; ++i) {
        const std::int64_t before = monotonic_raw_ns();
        const std::uint64_t ticks = read_stamp().ticks;
        const std::int64_t after = monotonic_raw_ns();
        const std::int64_t gap = after - before;
        if (best_gap < 0 || gap < best_gap) {
            best_gap = gap;
            best = PairedReading{
                .ticks = ticks, .ns = static_cast<double>(before) + static_cast<double>(gap) / 2.0};
        }
        if (gap <= kGoodGapNs) {
            break;
        }
    }
    return best;
}

// One round: how many counter ticks pass per nanosecond of the system clock over `wait_ns`.
inline double measure_rate(std::int64_t wait_ns) {
    const PairedReading first = paired_reading();
    timespec wait{.tv_sec = wait_ns / 1'000'000'000, .tv_nsec = wait_ns % 1'000'000'000};
    while (nanosleep(&wait, &wait) != 0) { // interrupted by a signal: sleep for what's left
    }
    const PairedReading second = paired_reading();
    return static_cast<double>(second.ticks - first.ticks) / (second.ns - first.ns);
}

// `rounds` rounds of `wait_ns` each. 5 rounds of 200 ms take about a second.
inline Calibration calibrate(int rounds = 5, std::int64_t wait_ns = 200'000'000) {
    assert(rounds > 0 && wait_ns > 0);
    std::vector<double> rates;
    rates.reserve(static_cast<std::size_t>(rounds));
    for (int i = 0; i < rounds; ++i) {
        rates.push_back(measure_rate(wait_ns));
    }
    std::ranges::sort(rates);
    const double median = rates[(rates.size() - 1) / 2];
    return Calibration{.ticks_per_ns = median, .spread = (rates.back() - rates.front()) / median};
}

// ---------------------------------------------------------------------------
// What the timer itself costs
// ---------------------------------------------------------------------------

struct Overhead {
    std::uint64_t min{0}; // the fixed part: subtracted from every sample (design §13 Q2)
    std::uint64_t median{0};
    std::uint64_t max{0};
};

// Times an empty region `samples` times (after a short untimed warm-up), in ticks. A pair whose
// two readings came from different cores is taken again.
inline Overhead measure_overhead(std::size_t samples = 1'000'000) {
    assert(samples > 0);
    for (int i = 0; i < 10'000; ++i) { // warm-up: brings the code into cache
        const Stamp t0 = start();
        const Stamp t1 = stop();
        static_cast<void>(t0);
        static_cast<void>(t1);
    }
    std::vector<std::uint64_t> durations(samples);
    std::size_t i = 0;
    while (i < samples) {
        const Stamp t0 = start();
        const Stamp t1 = stop();
        if (t0.core == t1.core) {
            durations[i++] = t1.ticks - t0.ticks;
        }
    }
    std::ranges::sort(durations);
    return Overhead{
        .min = durations.front(), .median = durations[(samples - 1) / 2], .max = durations.back()};
}

} // namespace nanobook::latency

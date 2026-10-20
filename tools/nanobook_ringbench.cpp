/*
 * nanobook_ringbench.cpp: measures one SPSC ring between two pinned threads (pipeline design §11).
 *
 * What it does: Two measurements of SpscRing, each with a producer and a consumer pinned to two
 *   different cores:
 *   - throughput: the producer pushes N numbers as fast as it can, the consumer pops them; reports
 *     records per second and how often each side waited;
 *   - round trip: thread A pushes a time stamp into a "ping" ring, thread B pops it and pushes it
 *     straight back through a "pong" ring, A pops it and records the time it took; one record in
 *     flight at a time, so there is never a queue. One-way latency is about half the round trip.
 *   Prints the environment block (rule 19) and the NOT FOR PUBLISHING banner where it applies.
 *     nanobook_ringbench [--cores A,B] [--count N] [--round-trips N]
 *
 * Why it exists: The ring is the pipeline's only link between threads. Its own cost (how long one
 *   record takes to cross from one core to another) is the floor under every pipeline latency.
 *
 * How it works: The phase 3 timer (support/tsc.hpp): calibrated, its own cost subtracted. Each
 *   round trip is one start()/stop() pair on thread A, so both stamps come from one core. The
 *   numbers are counted with the phase 3 Recorder, so the percentiles are exact.
 *
 * Trade-offs: A tool: it prints and allocates freely outside the timed loops. Pinning needs the
 *   two cores to be different physical cores (on this laptop CPUs 2 and 3 are one core's two
 *   hyperthreads), or the two threads share one core's caches and the numbers mean little.
 */

#include <sched.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#include "environment.hpp"
#include "latency_recorder.hpp"
#include "nanobook/pipeline/spsc_ring.hpp"
#include "tsc.hpp"

namespace {

using namespace nanobook::latency;
using nanobook::pipeline::SpscRing;

constexpr std::string_view kUsage =
    "usage: nanobook_ringbench [--cores A,B] [--count N] [--round-trips N]\n";
constexpr std::size_t kCapacity = 65'536; // the pipeline's ring size (design §3.4)

struct Options {
    int core_a{2};
    int core_b{4};
    std::uint64_t count{100'000'000};
    std::uint64_t round_trips{1'000'000};
};

template <typename T> bool parse_number(std::string_view text, T &value) {
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        bool ok = false;
        if (flag == "--cores") {
            const std::size_t comma = value.find(',');
            ok = comma != std::string_view::npos &&
                 parse_number(value.substr(0, comma), o.core_a) &&
                 parse_number(value.substr(comma + 1), o.core_b) && o.core_a >= 0 &&
                 o.core_b >= 0 && o.core_a != o.core_b;
        } else if (flag == "--count") {
            ok = parse_number(value, o.count) && o.count > 0;
        } else if (flag == "--round-trips") {
            ok = parse_number(value, o.round_trips) && o.round_trips > 0;
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    if (argc % 2 == 0) {
        return std::nullopt; // a flag without its value
    }
    return o;
}

bool pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 && sched_getcpu() == core;
}

// Both threads wait here until the other is pinned, so neither starts timing alone.
void wait_for(const std::atomic<int> &ready, int count) {
    while (ready.load(std::memory_order_acquire) < count) {
        nanobook::pipeline::cpu_relax();
    }
}

void print_ns(double ns) {
    std::cout << std::setw(10) << std::fixed << std::setprecision(ns < 1'000 ? 1 : 0) << ns;
}

} // namespace

int main(int argc, char **argv) {
    const std::optional<Options> parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << kUsage;
        return 2;
    }
    const Options &o = *parsed;
    if (!missing_tsc_flags(read_cpu_flags()).empty()) {
        std::cerr << "the time-stamp counter can't be used as a clock here\n";
        return 3;
    }
    const Environment env = collect_environment(o.core_a);
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
    std::cout << std::setw(17) << "cores" << o.core_a << " (producer, A), " << o.core_b
              << " (consumer, B)\n"
              << std::setw(17) << "ring" << kCapacity << " slots of 8 bytes\n"
              << std::right;

    // 1. Throughput: N numbers from A to B, as fast as possible.
    {
        auto ring = std::make_unique<SpscRing<std::uint64_t, kCapacity>>();
        std::atomic<int> ready{0};
        std::uint64_t producer_waits = 0;
        std::uint64_t consumer_waits = 0;
        std::uint64_t last = 0;
        Stamp t0{};
        Stamp t1{};
        bool pinned = true;
        {
            std::jthread producer{[&] {
                pinned = pinned && pin_to_core(o.core_a);
                ready.fetch_add(1, std::memory_order_acq_rel);
                wait_for(ready, 2);
                t0 = start();
                for (std::uint64_t i = 1; i <= o.count; ++i) {
                    producer_waits += ring->push(i);
                }
            }};
            std::jthread consumer{[&] {
                pinned = pinned && pin_to_core(o.core_b);
                ready.fetch_add(1, std::memory_order_acq_rel);
                wait_for(ready, 2);
                for (std::uint64_t i = 1; i <= o.count; ++i) {
                    consumer_waits += ring->pop(last);
                }
                t1 = stop();
            }};
        } // both threads joined here
        if (!pinned || last != o.count) {
            std::cerr << "could not pin to cores " << o.core_a << " and " << o.core_b
                      << ", or a record was lost\n";
            return 3;
        }
        const double ns = to_ns(t1.ticks - t0.ticks, rate);
        std::cout << std::fixed << "\nThroughput: " << o.count << " records in "
                  << std::setprecision(3) << ns / 1e9 << " s = " << std::setprecision(1)
                  << ns / static_cast<double>(o.count) << " ns per record, "
                  << static_cast<double>(o.count) / ns * 1e3
                  << " M records/s\n  producer found the ring full " << producer_waits
                  << " times; consumer found it empty " << consumer_waits << " times\n";
    }

    // 2. Round trip: one record in flight, A -> B -> A.
    {
        auto ping = std::make_unique<SpscRing<std::uint64_t, kCapacity>>();
        auto pong = std::make_unique<SpscRing<std::uint64_t, kCapacity>>();
        Recorder recorder{o.round_trips};
        std::atomic<int> ready{0};
        {
            std::jthread echo{[&] { // B: sends back whatever arrives
                static_cast<void>(pin_to_core(o.core_b));
                ready.fetch_add(1, std::memory_order_acq_rel);
                wait_for(ready, 2);
                for (std::uint64_t i = 0; i < o.round_trips + 1'000; ++i) {
                    std::uint64_t value = 0;
                    ping->pop(value);
                    pong->push(value);
                }
            }};
            static_cast<void>(pin_to_core(o.core_a)); // this thread is A
            ready.fetch_add(1, std::memory_order_acq_rel);
            wait_for(ready, 2);
            for (std::uint64_t i = 0; i < o.round_trips + 1'000; ++i) {
                std::uint64_t back = 0;
                const Stamp s0 = start();
                ping->push(i);
                pong->pop(back);
                const Stamp s1 = stop();
                if (i >= 1'000) { // the first 1,000 warm the caches and the branch predictors
                    recorder.record(s0, s1, Tag::Other);
                }
            }
        }
        const Summary s = summarize(recorder, overhead.min, rate, std::nullopt);
        std::cout << "\nRound trip A -> B -> A (" << s.count << " samples; ns, timer overhead "
                  << "subtracted; one way is about half)\n"
                  << "         min       p50       p90       p99     p99.9    p99.99       max\n";
        for (const double v : {s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns, s.p999_ns}) {
            print_ns(v);
        }
        if (s.count >= 1'000'000) { // p99.99 rests on at least 100 samples (phase 3's rule)
            print_ns(s.p9999_ns);
        } else {
            std::cout << std::setw(10) << "-";
        }
        print_ns(s.max_ns);
        std::cout << "\n  dropped (moved core) " << recorder.dropped() << "\n";
    }
    if (!env.publishable()) {
        std::cout << banner << "\n";
    }
    return 0;
}

/*
 * pacing.hpp: sends a stream out on a schedule, and stamps each record with its intended time.
 *
 * What it does: Pacer is the pipeline's stamp for the paced latency runs (pipeline design §9.2).
 *   Records before a chosen ITCH time go out at once, unstamped (the warm-up that builds the
 *   books). From that time on, after a settle time that lets the engine drain the warm-up's
 *   backlog, record n is given its intended send time, start + n x (ticks per second / rate),
 *   and the feed waits until the clock reaches it before sending. The record carries the
 *   intended time, not the moment it actually left. ClockPacer is the same idea on the ITCH
 *   clock instead (viewer design §5): a record stamped t ns after the chosen time is due t / speed
 *   after the start, so the day keeps its shape (the open's burst, the quiet lunch) at 1x, 60x
 *   or any whole-number speed, for watching it in nanobook_view.
 *
 * Why it exists: Rule 18, coordinated omission. If the feed stamped the moment it actually sent a
 *   record, a stall would hide itself: a record due at t = 0 but sent at t = 50 µs would claim
 *   0 µs of waiting. Stamping the intended time counts that 50 µs, as a real market would: the
 *   exchange doesn't wait for us.
 *
 * How it works: Whole-number arithmetic: n x ticks_per_second / rate, so no rounding error
 *   builds up over millions of records (n x ticks_per_second stays below 2^64 for n up to about
 *   7 billion at 2.4 GHz). Waiting spins with the CPU's pause instruction. The clock is a template
 *   parameter: the time-stamp counter in the tool, a fake clock in the tests.
 *   ClockPacer converts each record's distance from the start (ITCH ns) to ticks, every time from
 *   the start, so nothing drifts either: whole seconds x ticks_per_second, plus the rest of a
 *   second x ticks_per_second / 10^9 (the rest is below 10^9, so the product stays below 2^64 up
 *   to 18 GHz), then / speed. A record whose ITCH time is earlier than the last one's is due at
 *   the last one's time: the schedule never goes back.
 *
 * Trade-offs: The feed thread spins while it waits, so it needs its own core (it has one). Tool
 *   and test code.
 */

#pragma once

#include <algorithm>
#include <cstdint>

#include "nanobook/pipeline/records.hpp"
#include "nanobook/pipeline/spsc_ring.hpp"

namespace nanobook::pipeline {

template <typename Clock> class Pacer {
public:
    // `from_ns`: ITCH time (nanoseconds since midnight) the paced part starts at.
    // `settle_ticks`: how long to wait after the warm-up before the first paced record, so the
    // engine drains the backlog the unpaced warm-up left in the rings (up to 65,536 records):
    // without it the first samples measure that backlog, not the pipeline.
    Pacer(Clock clock, std::uint64_t ticks_per_second, std::uint64_t records_per_second,
          std::uint64_t from_ns, std::uint64_t settle_ticks = 0)
        : clock_(clock), ticks_per_second_(ticks_per_second), rate_(records_per_second),
          from_ns_(from_ns), settle_ticks_(settle_ticks) {
    }

    // The stamp for one record: 0 during the warm-up, else its intended send time (after
    // waiting for it).
    std::uint64_t operator()(const FeedRecord &r) {
        if (!started_) {
            if (r.timestamp < from_ns_) {
                return 0; // warm-up: not paced, not measured
            }
            started_ = true;
            start_ = clock_() + settle_ticks_;
        }
        const std::uint64_t intended = start_ + paced_ * ticks_per_second_ / rate_;
        ++paced_;
        while (clock_() < intended) {
            cpu_relax();
        }
        return intended;
    }

    [[nodiscard]] std::uint64_t paced() const {
        return paced_;
    }

private:
    Clock clock_;
    std::uint64_t ticks_per_second_;
    std::uint64_t rate_;
    std::uint64_t from_ns_;
    std::uint64_t settle_ticks_;
    bool started_{false};
    std::uint64_t start_{0};
    std::uint64_t paced_{0};
};

// Paces by the ITCH clock (viewer design §5): `speed` ITCH seconds go by per real second.
template <typename Clock> class ClockPacer {
public:
    // `from_ns` and `settle_ticks` as for Pacer. `speed` >= 1.
    ClockPacer(Clock clock, std::uint64_t ticks_per_second, std::uint64_t speed,
               std::uint64_t from_ns, std::uint64_t settle_ticks = 0)
        : clock_(clock), ticks_per_second_(ticks_per_second), speed_(speed), from_ns_(from_ns),
          settle_ticks_(settle_ticks) {
    }

    // The stamp for one record: 0 during the warm-up, else its intended send time (after
    // waiting for it).
    std::uint64_t operator()(const FeedRecord &r) {
        if (!started_) {
            if (r.timestamp < from_ns_) {
                return 0; // warm-up: not paced, not measured
            }
            started_ = true;
            start_ = clock_() + settle_ticks_;
        }
        constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
        const std::uint64_t elapsed_ns = r.timestamp > from_ns_ ? r.timestamp - from_ns_ : 0;
        const std::uint64_t ticks = (elapsed_ns / kNsPerSecond * ticks_per_second_) +
                                    (elapsed_ns % kNsPerSecond * ticks_per_second_ / kNsPerSecond);
        due_ = std::max(due_, start_ + ticks / speed_);
        ++paced_;
        while (clock_() < due_) {
            cpu_relax();
        }
        return due_;
    }

    [[nodiscard]] std::uint64_t paced() const {
        return paced_;
    }

private:
    Clock clock_;
    std::uint64_t ticks_per_second_;
    std::uint64_t speed_;
    std::uint64_t from_ns_;
    std::uint64_t settle_ticks_;
    bool started_{false};
    std::uint64_t start_{0};
    std::uint64_t due_{0};
    std::uint64_t paced_{0};
};

} // namespace nanobook::pipeline

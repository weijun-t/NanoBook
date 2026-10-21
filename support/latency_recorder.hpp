/*
 * latency_recorder.hpp: stores every timed operation's duration and turns them into percentiles.
 *
 * What it does: Recorder keeps one raw sample per timed operation: its duration in counter ticks
 *   and a tag saying what kind of operation it was (record_across_cores() for a duration whose two
 *   ends were read on different cores: the pipeline's end-to-end latency). time_each() is the
 *   timing loop: it times one
 *   call per sample with tsc.hpp's start()/stop(). After the run, summarize() gives min, p50, p90,
 *   p99, p99.9, p99.99, max and mean in nanoseconds (all samples, or one tag's), and histogram()
 *   groups the samples into buckets for the plot.
 *
 * Why it exists: Phase 3 reports the whole distribution, not the average (latency-harness design
 *   §2, §5). Raw samples were chosen over a histogram-only recorder (design §13 Q1): exact
 *   percentiles, and the order the operations happened in is kept.
 *
 * How it works:
 *   - record(): O(1). Writes 4 + 1 bytes at the next position of two arrays allocated (and
 *     touched) in the constructor, so recording never allocates or page-faults. A sample whose
 *     two stamps came from different cores is dropped and counted; one too long for 32 bits is
 *     clamped and counted; a sample beyond the capacity is refused and counted.
 *   - nearest_rank(): the p-th percentile is the sample at position ceil(p × N) − 1 of the sorted
 *     samples, so every reported value is a real sample. p is given in millionths (999'000 is
 *     p99.9) and the rank is computed in integers, so no floating-point rounding can pick the
 *     wrong position. O(1) on sorted data.
 *   - summarize(): copies the selected samples and sorts them (O(n log n), after the run),
 *     subtracts the timer overhead's minimum from each (never below 0), converts to ns.
 *   - histogram(): HdrHistogram-style buckets: 1 ns wide below 1,024 ns, then every doubling
 *     (1,024–2,047, 2,048–4,095, …) split into 64 equal buckets, so a bucket is at most 1/64
 *     (about 1.6%) of its values wide. Finding a bucket is a few instructions (std::bit_width).
 *
 * Data structures: Two std::vectors sized once: ticks (uint32_t) and tags (uint8_t), 5 bytes per
 *   sample: 5 MB per million.
 *
 * Trade-offs: 32-bit ticks hold up to about 1.7 s at this laptop's 2.4 GHz; longer samples are
 *   clamped (and counted), which only an extreme pause could cause. Tool and test code: summarize()
 *   and histogram() allocate freely, but only after the timed loop.
 */

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "tsc.hpp"

namespace nanobook::latency {

// ---------------------------------------------------------------------------
// Tags: what kind of operation a sample was (design §5.3)
// ---------------------------------------------------------------------------

// Replace: an ITCH Order Replace (U), a cancel-replace that loses its place (phase 4).
enum class Tag : std::uint8_t { Add, Match, Cancel, Modify, Replace, Reject, Other };

inline constexpr std::array<Tag, 7> kAllTags = {Tag::Add,     Tag::Match,  Tag::Cancel, Tag::Modify,
                                                Tag::Replace, Tag::Reject, Tag::Other};

constexpr std::string_view tag_name(Tag tag) {
    switch (tag) {
    case Tag::Add:
        return "add";
    case Tag::Match:
        return "match";
    case Tag::Cancel:
        return "cancel";
    case Tag::Modify:
        return "modify";
    case Tag::Replace:
        return "replace";
    case Tag::Reject:
        return "reject";
    case Tag::Other:
        return "other";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

class Recorder {
public:
    // Allocates room for `capacity` samples. std::vector's zero-fill writes every byte, so every
    // memory page is touched here and none is first touched while recording.
    explicit Recorder(std::size_t capacity) : ticks_(capacity), tags_(capacity) {
    }

    // Hot path of the harness: no allocation, one predictable branch per check.
    void record(Stamp t0, Stamp t1, Tag tag) {
        if (t0.core != t1.core) { // the thread moved mid-operation: the sample means nothing
            ++dropped_;
            return;
        }
        if (size_ == ticks_.size()) {
            ++refused_;
            return;
        }
        std::uint64_t ticks = t1.ticks - t0.ticks;
        if (ticks > std::numeric_limits<std::uint32_t>::max()) {
            ticks = std::numeric_limits<std::uint32_t>::max();
            ++clamped_;
        }
        ticks_[size_] = static_cast<std::uint32_t>(ticks);
        tags_[size_] = tag;
        ++size_;
    }

    // A duration whose two ends were read on different cores (the pipeline's end-to-end latency:
    // stamped on the feed thread, read on the publisher's). Valid because this CPU's time-stamp
    // counter is invariant and the same on every core (constant_tsc, nonstop_tsc).
    void record_across_cores(std::uint64_t ticks, Tag tag) {
        if (size_ == ticks_.size()) {
            ++refused_;
            return;
        }
        if (ticks > std::numeric_limits<std::uint32_t>::max()) {
            ticks = std::numeric_limits<std::uint32_t>::max();
            ++clamped_;
        }
        ticks_[size_] = static_cast<std::uint32_t>(ticks);
        tags_[size_] = tag;
        ++size_;
    }

    void clear() {
        size_ = dropped_ = clamped_ = refused_ = 0;
    }

    std::size_t size() const {
        return size_;
    }
    std::size_t capacity() const {
        return ticks_.size();
    }
    std::uint64_t dropped() const {
        return dropped_;
    } // stamps from different cores
    std::uint64_t clamped() const {
        return clamped_;
    } // longer than 32 bits of ticks
    std::uint64_t refused() const {
        return refused_;
    } // recorded past the capacity

    // The samples in the order they were recorded.
    std::span<const std::uint32_t> ticks() const {
        return {ticks_.data(), size_};
    }
    std::span<const Tag> tags() const {
        return {tags_.data(), size_};
    }

private:
    std::vector<std::uint32_t> ticks_;
    std::vector<Tag> tags_;
    std::size_t size_{0};
    std::uint64_t dropped_{0};
    std::uint64_t clamped_{0};
    std::uint64_t refused_{0};
};

// The timing loop (design §6.1). For i = 0 .. n-1: times work(i) alone, then calls after(i)
// untimed, which returns the sample's Tag (and may reset state, e.g. clear an event buffer).
template <typename Work, typename After>
void time_each(Recorder &recorder, std::size_t n, Work &&work, After &&after) {
    for (std::size_t i = 0; i < n; ++i) {
        const Stamp t0 = start();
        work(i);
        const Stamp t1 = stop();
        recorder.record(t0, t1, after(i));
    }
}

// ---------------------------------------------------------------------------
// Percentiles
// ---------------------------------------------------------------------------

// Percentiles in millionths, so the rank arithmetic stays in integers.
inline constexpr std::uint64_t kP50 = 500'000;
inline constexpr std::uint64_t kP90 = 900'000;
inline constexpr std::uint64_t kP99 = 990'000;
inline constexpr std::uint64_t kP999 = 999'000;
inline constexpr std::uint64_t kP9999 = 999'900;
inline constexpr std::uint64_t kMax = 1'000'000;

// Nearest rank: the sample at position ceil(q × N) − 1 of `sorted` (q in millionths). q = 0 gives
// the smallest sample, q = 1'000'000 the largest. `sorted` must be non-empty and sorted.
template <typename T> T nearest_rank(std::span<const T> sorted, std::uint64_t q_millionths) {
    assert(!sorted.empty() && q_millionths <= kMax);
    const std::uint64_t n = sorted.size();
    const std::uint64_t rank = (q_millionths * n + kMax - 1) / kMax; // ceil(q × n / 1e6)
    return sorted[std::max<std::uint64_t>(rank, 1) - 1];
}

// The fixed timer cost is subtracted from each sample, never going below 0.
inline std::uint64_t minus_overhead(std::uint64_t ticks, std::uint64_t overhead) {
    return ticks > overhead ? ticks - overhead : 0;
}

struct Summary {
    std::size_t count{0};
    double min_ns{0};
    double p50_ns{0};
    double p90_ns{0};
    double p99_ns{0};
    double p999_ns{0};
    double p9999_ns{0}; // only meaningful with 1,000,000+ samples (design §5.2)
    double max_ns{0};
    double mean_ns{0};
};

// The samples with tag `only` (or all, if empty), sorted, overhead subtracted.
inline std::vector<std::uint64_t> sorted_samples(const Recorder &recorder,
                                                 std::uint64_t overhead_ticks,
                                                 std::optional<Tag> only = std::nullopt) {
    std::vector<std::uint64_t> out;
    out.reserve(recorder.size());
    const auto ticks = recorder.ticks();
    const auto tags = recorder.tags();
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        if (!only || tags[i] == *only) {
            out.push_back(minus_overhead(ticks[i], overhead_ticks));
        }
    }
    std::ranges::sort(out);
    return out;
}

// Percentiles in ns of the samples with tag `only` (or all). count == 0 means no such samples.
inline Summary summarize(const Recorder &recorder, std::uint64_t overhead_ticks,
                         double ticks_per_ns, std::optional<Tag> only = std::nullopt) {
    const std::vector<std::uint64_t> s = sorted_samples(recorder, overhead_ticks, only);
    if (s.empty()) {
        return Summary{};
    }
    const std::span<const std::uint64_t> sorted{s};
    const auto ns = [&](std::uint64_t q) { return to_ns(nearest_rank(sorted, q), ticks_per_ns); };
    double total = 0;
    for (const std::uint64_t t : s) {
        total += static_cast<double>(t);
    }
    return Summary{.count = s.size(),
                   .min_ns = ns(0),
                   .p50_ns = ns(kP50),
                   .p90_ns = ns(kP90),
                   .p99_ns = ns(kP99),
                   .p999_ns = ns(kP999),
                   .p9999_ns = ns(kP9999),
                   .max_ns = ns(kMax),
                   .mean_ns = total / static_cast<double>(s.size()) / ticks_per_ns};
}

// ---------------------------------------------------------------------------
// Histogram (design §5.4)
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kExactBelow = 1'024; // 1 ns buckets below this
inline constexpr std::uint64_t kSubBuckets = 64;    // buckets per doubling above it

// Which bucket `ns` falls in. 0 .. 1023 are their own bucket; above that, a doubling
// [2^e, 2^(e+1)) is split into 64 buckets of width 2^(e-6).
inline std::size_t bucket_of(std::uint64_t ns) {
    if (ns < kExactBelow) {
        return static_cast<std::size_t>(ns);
    }
    const auto e = static_cast<std::uint64_t>(std::bit_width(ns) - 1); // 2^e <= ns < 2^(e+1)
    const std::uint64_t sub = (ns - (std::uint64_t{1} << e)) >> (e - 6);
    return static_cast<std::size_t>(kExactBelow + (e - 10) * kSubBuckets + sub);
}

// The smallest ns value in bucket `b` (the inverse of bucket_of).
inline std::uint64_t bucket_low(std::size_t b) {
    if (b < kExactBelow) {
        return b;
    }
    const std::uint64_t e = 10 + (b - kExactBelow) / kSubBuckets;
    const std::uint64_t sub = (b - kExactBelow) % kSubBuckets;
    return (std::uint64_t{1} << e) + (sub << (e - 6));
}

struct Bucket {
    std::uint64_t low_ns{0};  // inclusive
    std::uint64_t high_ns{0}; // inclusive
    std::uint64_t count{0};
};

// Groups `sorted_ticks` (sorted, e.g. from sorted_samples()) into buckets of whole nanoseconds
// (rounded down). Only non-empty buckets are returned, lowest first. O(n): the input is sorted,
// so each bucket's samples arrive together.
inline std::vector<Bucket> histogram(std::span<const std::uint64_t> sorted_ticks,
                                     double ticks_per_ns) {
    std::vector<Bucket> out;
    for (const std::uint64_t ticks : sorted_ticks) {
        const auto ns = static_cast<std::uint64_t>(to_ns(ticks, ticks_per_ns));
        const std::size_t b = bucket_of(ns);
        if (out.empty() || bucket_of(out.back().low_ns) != b) {
            out.push_back(Bucket{.low_ns = bucket_low(b), .high_ns = bucket_low(b + 1) - 1});
        }
        ++out.back().count;
    }
    return out;
}

} // namespace nanobook::latency

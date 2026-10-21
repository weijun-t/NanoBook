/*
 * page_faults.hpp: counts the page faults this process has taken, for timing a piece of work.
 *
 * What it does: page_faults_now() reads how many minor and major page faults the whole process
 *   (every thread) has taken so far. Read it before and after a piece of work; the difference is
 *   that work's faults.
 *
 * Why it exists: Phase 6 experiments E2 and E6 (docs/optimise-design.md §2) test whether page
 *   faults cause the slow tail. A page fault is the operating system stopping the program to give
 *   it a page of memory the first time it is touched: "minor" if the page only needs mapping
 *   (fresh zeroed memory, or a file page already in memory), "major" if it must be read from disk.
 *   A minor fault costs microseconds, enough to show at p99.99. This counter works on WSL2, where
 *   the hardware counters don't (design §0.3).
 *
 * How it works: getrusage(RUSAGE_SELF) fills ru_minflt and ru_majflt, which the kernel keeps for
 *   the process: threads that have already ended are included. One system call per reading, so
 *   it is read outside the timed part, never inside it.
 *
 * Trade-offs: Tool and test code only (a system call, rule 6). Counts the whole process, so a
 *   reading is only meaningful while no other thread of this process is doing unrelated work.
 */

#pragma once

#include <sys/resource.h>

#include <cstdint>

namespace nanobook::latency {

struct PageFaults {
    std::uint64_t minor{}; // the page was mapped without reading the disk
    std::uint64_t major{}; // the page had to be read from disk

    PageFaults operator-(const PageFaults &earlier) const {
        return {.minor = minor - earlier.minor, .major = major - earlier.major};
    }
    PageFaults &operator+=(const PageFaults &more) {
        minor += more.minor;
        major += more.major;
        return *this;
    }
};

// The process's page faults so far (all threads). Zero if the kernel can't say.
inline PageFaults page_faults_now() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return {};
    }
    return {.minor = static_cast<std::uint64_t>(usage.ru_minflt),
            .major = static_cast<std::uint64_t>(usage.ru_majflt)};
}

} // namespace nanobook::latency

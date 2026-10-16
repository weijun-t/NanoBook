/*
 * alloc_counter.hpp: counts every heap allocation in the program.
 *
 * What it does: Every time any code asks for heap memory (`new`, a std::vector growing, a std::map
 *   adding a node), a counter goes up by one. Tests and benchmarks read it before and after some
 *   code to see how many allocations that code made.
 *
 * Why it exists: Rule 1 says the fast book must not allocate while trading, because asking for
 *   memory is slow and unpredictable. This turns "zero allocations" from a claim into a test.
 *
 * How it works: C++ lets a program replace the global operator new, the one function every
 *   allocation goes through. The replacement (alloc_counter.cpp) adds 1 to a counter, then calls
 *   malloc as usual. allocations_during(code) returns the count after minus the count before.
 *
 * Data structures: One std::atomic<uint64_t>. Atomic in case benchmark helper threads allocate;
 *   relaxed ordering because only the total matters.
 *
 * Trade-offs: It replaces new for the whole program, so it is only linked into the benchmarks and
 *   the zero-allocation tests, never the engine or sanitizer builds. The compiler may delete an
 *   allocation whose memory is never used, so tests mark memory as used with keep(), or they
 *   count too few.
 */

#pragma once

#include <cstdint>
#include <utility>

namespace nanobook::support {

// How many times global operator new has been called since the program started.
std::uint64_t allocations();

// Runs `work` and returns how many allocations it made.
template <typename F> std::uint64_t allocations_during(F &&work) {
    const std::uint64_t before = allocations();
    std::forward<F>(work)();
    return allocations() - before;
}

} // namespace nanobook::support

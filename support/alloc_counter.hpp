#pragma once

#include <cstdint>
#include <utility>

namespace nanobook::support {

// How many times global operator new has been called since the program started.
//
// alloc_counter.cpp replaces the global operator new, so this file only belongs in programs that
// measure allocations: the benchmarks and the zero-allocation tests. Never the engine library,
// and never a sanitizer build (ASan and TSan track allocations themselves). README rule 1:
// the fast book must make 0 allocations while trading.
std::uint64_t allocations();

// Runs `work` and returns how many allocations it made.
template <typename F> std::uint64_t allocations_during(F &&work) {
    const std::uint64_t before = allocations();
    std::forward<F>(work)();
    return allocations() - before;
}

} // namespace nanobook::support

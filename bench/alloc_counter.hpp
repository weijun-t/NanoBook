#pragma once

#include <cstdint>

namespace nanobook::bench {

// How many times global operator new has been called since the program started.
// alloc_counter.cpp replaces operator new for the benchmark binary only (never the engine), so
// a benchmark can read this before and after its timed work to get "allocations per operation"
// (design §13). The phase 2 fast book must reach exactly 0 (README rule 1).
std::uint64_t allocations();

} // namespace nanobook::bench

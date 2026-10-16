/*
 * alloc_counter.cpp: the replacement operator new and delete that do the counting.
 *
 * What it does: Defines the global operator new, new[], delete and delete[]. Each new adds 1 to an
 *   atomic counter, then gets the memory from malloc. Each delete gives it back to free.
 *
 * Why it exists: See alloc_counter.hpp. The standard allows a program to replace these functions:
 *   defining them makes every `new` in the program call them.
 *
 * Trade-offs: Only the benchmarks and the zero-allocation tests link this file, through the CMake
 *   OBJECT target nanobook_alloc_counter (an OBJECT library, so the linker can't skip it). The
 *   engine library, the main test program and sanitizer builds keep the normal ones. Aligned new
 *   is not replaced, because nothing in the engine uses over-aligned types yet.
 */

#include "alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// Atomic because Google Benchmark may run helper threads; relaxed because only the count matters,
// not its ordering with other memory.
std::atomic<std::uint64_t> g_allocations{0};

void *counted_alloc(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(size == 0 ? 1 : size)) { // new must return a unique pointer for 0
        return p;
    }
    throw std::bad_alloc(); // what the standard requires of a failing operator new
}

} // namespace

std::uint64_t nanobook::support::allocations() {
    return g_allocations.load(std::memory_order_relaxed);
}

void *operator new(std::size_t size) {
    return counted_alloc(size);
}
void *operator new[](std::size_t size) {
    return counted_alloc(size);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t /*size*/) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t /*size*/) noexcept {
    std::free(p);
}

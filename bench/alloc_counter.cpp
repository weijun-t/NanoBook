#include "alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

// Replacing the global operator new/delete is allowed by the standard: defining these functions
// in a program makes every `new` in that program call them. Only the benchmark binary links this
// file. The tests and the engine library keep the normal ones, and so does ASan (it has its own).

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

std::uint64_t nanobook::bench::allocations() {
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

/*
 * prefault_allocator.hpp: an allocator that touches every page of its memory as it hands it out.
 *
 * What it does: PrefaultAllocator<T> is std::allocator<T> plus one step: after getting the
 *   memory, it writes a byte into every 4 KiB page of it. A std::vector using it, reserved at
 *   start-up, then has all its pages mapped before the first element is ever stored.
 *
 * Why it exists: Phase 6 experiment E2 (optimise design §2). The operating system maps a page of
 *   new memory only when the program first touches it, stopping the program for a few
 *   microseconds (a minor page fault). SortedLevels only reserves its arrays, so a page was first
 *   touched mid-replay, when a book grew: a possible cause of the slow tail. Touching every page
 *   at construction moves those faults to start-up, where rule 1 says the costs belong.
 *
 * How it works: allocate(n) asks std::allocator for n * sizeof(T) bytes, then writes 0 at every
 *   4,096th byte and at the last byte, through a volatile pointer so the compiler can't drop the
 *   writes. Every page the range spans gets one write: the writes are one page apart, starting
 *   at the first byte. These are writes to raw storage, before any T exists, through unsigned
 *   char, which may alias anything. Cost: one write per page at start-up, O(bytes / 4 KiB).
 *
 * Trade-offs: The whole reservation becomes resident memory, used or not (for the real day's
 *   books, about 161 MB instead of about 20 MB actually touched). Nothing else changes: it is
 *   stateless, every instance compares equal, and the vector's element code is the same.
 */

#pragma once

#include <cstddef>
#include <memory>

namespace nanobook::fast {

template <typename T> class PrefaultAllocator {
public:
    using value_type = T;

    PrefaultAllocator() = default;
    template <typename U>
    explicit(false) PrefaultAllocator(const PrefaultAllocator<U> & /*other*/) {
    }

    [[nodiscard]] T *allocate(std::size_t n) {
        T *memory = std::allocator<T>{}.allocate(n);
        const std::size_t size = n * sizeof(T);
        if (size != 0) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): raw storage as bytes
            auto *bytes = reinterpret_cast<volatile unsigned char *>(memory);
            for (std::size_t i = 0; i < size; i += kPageBytes) {
                bytes[i] = 0;
            }
            bytes[size - 1] = 0; // the last page, if the range doesn't start on a page boundary
        }
        return memory;
    }

    void deallocate(T *memory, std::size_t n) noexcept {
        std::allocator<T>{}.deallocate(memory, n);
    }

    template <typename U> bool operator==(const PrefaultAllocator<U> & /*other*/) const noexcept {
        return true; // stateless: memory from one can be freed by any other
    }

private:
    static constexpr std::size_t kPageBytes = 4096;
};

} // namespace nanobook::fast

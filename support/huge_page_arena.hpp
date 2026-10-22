/*
 * huge_page_arena.hpp: one start-up region on 2 MiB pages that the books' arrays are built in.
 *
 * What it does: HugePageArena maps one region, aligned to 2 MiB, asks the kernel to back it with
 *   2 MiB "huge" pages, touches every page, and hands its bytes out through a standard
 *   std::pmr::memory_resource. DefaultResourceScope makes that resource the default while the
 *   books are built, so their std::pmr::vectors (order pool, id table, levels) take their memory
 *   from it. build_books() does both, sized by arena_bytes() from the books' own numbers.
 *
 * Why it exists: Optimise design E6. The CPU translates every address through a small cache of
 *   page translations (the TLB). The real day's books take about 400 MB: about 100,000 pages of
 *   4 KiB, far more than the TLB holds, so many messages wait for a page-table walk. On 2 MiB
 *   pages it is about 200. Tried first with glibc's switch (GLIBC_TUNABLES=glibc.malloc.hugetlb=1):
 *   4.8% more throughput and p99 7.6% better. This arena makes it part of the program.
 *
 * How it works:
 *   - mmap the size rounded up to 2 MiB, plus 2 MiB of slack, and start at the first 2 MiB
 *     boundary inside it: a huge page must start on one.
 *   - madvise(MADV_HUGEPAGE): this kernel gives huge pages only to regions that ask
 *     (transparent_hugepage = madvise). If it refuses, the pages are normal ones; huge_bytes()
 *     reports how much really landed on huge pages (the AnonHugePages count grows by that much).
 *   - One write per 4 KiB page now, at start-up (rule 1), so no page is first touched while
 *     trading: this also does what E2's PrefaultAllocator did.
 *   - std::pmr::monotonic_buffer_resource hands out the bytes in order and frees nothing until
 *     the arena goes: the books live as long as it does. Its upstream is null_memory_resource, so
 *     a wrong size throws std::bad_alloc instead of quietly falling back to normal pages.
 *
 * Data structures: the mapping (base, length, aligned start, usable size) and the monotonic
 *   resource on top of it.
 *
 * Trade-offs: Linux only, tool code. The whole region is resident from the start. The arena must
 *   outlive everything built in it. DefaultResourceScope changes a process-wide default, so only
 *   the thread building the books may run while it is active.
 */

#pragma once

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory_resource>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/fast_book.hpp"

namespace nanobook::support {

inline constexpr std::size_t kHugePage = std::size_t{2} << 20U; // 2 MiB
inline constexpr std::size_t kPage = 4096;

// This process's memory on transparent huge pages, in bytes (0 if unknown).
inline std::uint64_t anon_huge_bytes() {
    std::ifstream rollup{"/proc/self/smaps_rollup"};
    std::string key;
    std::uint64_t kilobytes = 0;
    while (rollup >> key) {
        if (key == "AnonHugePages:" && rollup >> kilobytes) {
            return kilobytes * 1024;
        }
    }
    return 0;
}

class HugePageArena {
public:
    explicit HugePageArena(std::size_t bytes)
        : mapping_(map(bytes)),
          resource_(mapping_.start, mapping_.size, std::pmr::null_memory_resource()) {
        const std::uint64_t before = anon_huge_bytes();
        volatile auto *page =
            static_cast<unsigned char *>(mapping_.start); // writes can't be dropped
        for (std::size_t i = 0; i < mapping_.size; i += kPage) {
            page[i] = 0;
        }
        huge_bytes_ = anon_huge_bytes() - before;
    }
    HugePageArena(const HugePageArena &) = delete;
    HugePageArena &operator=(const HugePageArena &) = delete;
    HugePageArena(HugePageArena &&) = delete;
    HugePageArena &operator=(HugePageArena &&) = delete;
    ~HugePageArena() {
        munmap(mapping_.base, mapping_.length);
    }

    [[nodiscard]] std::pmr::memory_resource *resource() {
        return &resource_;
    }
    // Usable bytes: the request rounded up to 2 MiB.
    [[nodiscard]] std::size_t size() const {
        return mapping_.size;
    }
    // How many of them the kernel backed with 2 MiB pages.
    [[nodiscard]] std::uint64_t huge_bytes() const {
        return huge_bytes_;
    }
    [[nodiscard]] bool contains(const void *p) const {
        const auto *start = static_cast<const unsigned char *>(mapping_.start);
        const auto *byte = static_cast<const unsigned char *>(p);
        return byte >= start && byte < start + mapping_.size;
    }

private:
    struct Mapping {
        void *base{};
        std::size_t length{};
        void *start{}; // the first 2 MiB boundary in [base, base + length)
        std::size_t size{};
    };

    static Mapping map(std::size_t bytes) {
        Mapping m;
        m.size = (std::max<std::size_t>(bytes, 1) + kHugePage - 1) / kHugePage * kHugePage;
        m.length = m.size + kHugePage; // room to move the start to a 2 MiB boundary
        m.base =
            mmap(nullptr, m.length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m.base == MAP_FAILED) {
            throw std::bad_alloc{};
        }
        const auto address = reinterpret_cast<std::uintptr_t>(m.base); // NOLINT: address maths
        const std::uintptr_t aligned = (address + kHugePage - 1) & ~(kHugePage - 1);
        m.start = static_cast<unsigned char *>(m.base) + (aligned - address);
        madvise(m.start, m.size, MADV_HUGEPAGE); // a refusal leaves normal pages: huge_bytes() says
        return m;
    }

    Mapping mapping_;
    std::pmr::monotonic_buffer_resource resource_;
    std::uint64_t huge_bytes_{};
};

// Makes `resource` the default memory resource until the end of the scope.
class DefaultResourceScope {
public:
    explicit DefaultResourceScope(std::pmr::memory_resource *resource)
        : previous_(std::pmr::set_default_resource(resource)) {
    }
    DefaultResourceScope(const DefaultResourceScope &) = delete;
    DefaultResourceScope &operator=(const DefaultResourceScope &) = delete;
    DefaultResourceScope(DefaultResourceScope &&) = delete;
    DefaultResourceScope &operator=(DefaultResourceScope &&) = delete;
    ~DefaultResourceScope() {
        std::pmr::set_default_resource(previous_);
    }

private:
    std::pmr::memory_resource *previous_;
};

// Bytes for one FastBook per config: the arrays themselves, plus up to 64 bytes of alignment per
// array (the monotonic resource aligns each one; 4 arrays per book).
inline std::size_t arena_bytes(const std::vector<BookConfig> &configs) {
    std::size_t total = 0;
    for (const BookConfig &config : configs) {
        total += FastBook::arena_bytes(config) + (std::size_t{4} * 64);
    }
    return total;
}

// "412 MB arena, 412 MB of it on 2 MiB pages": for the timed tools' reports.
inline std::string arena_text(const HugePageArena &arena) {
    return std::to_string(arena.size() >> 20U) + " MB arena, " +
           std::to_string(arena.huge_bytes() >> 20U) + " MB of it on 2 MiB pages";
}

// Builds a Builder (a BookBuilder<FastBook>) with its books' arrays in `arena`.
template <typename Builder, typename... Args>
Builder build_books(HugePageArena &arena, Args &&...args) {
    const DefaultResourceScope scope{arena.resource()};
    return Builder{std::forward<Args>(args)...};
}

} // namespace nanobook::support

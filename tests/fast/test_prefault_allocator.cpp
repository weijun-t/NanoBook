/*
 * test_prefault_allocator.cpp: tests for the page-touching allocator
 * (include/nanobook/fast/prefault_allocator.hpp).
 *
 * What it does: After allocate(), writing to every page of the memory must cost (almost) no page
 *   fault, including the last page of a range that doesn't start on a page boundary: allocate()
 *   has already touched them all. A std::vector using it must behave like any vector, and two
 *   allocators must compare equal (they're stateless).
 *
 * Why it exists: Phase 6 experiment E2 relies on it to move the level arrays' page faults to
 *   start-up (optimise design §2). An allocator that missed a page would leave a fault mid-replay.
 *
 * How it works: The page-fault counter (support/page_faults.hpp) is read around the writes. The
 *   memory is exactly 1 MiB, which usually starts 16 bytes past a page boundary, so its last page
 *   is a partial one that only the allocator's last-byte write reaches.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "nanobook/fast/level.hpp"
#include "nanobook/fast/prefault_allocator.hpp"
#include "page_faults.hpp"

using nanobook::fast::Level;
using nanobook::fast::PrefaultAllocator;
using nanobook::latency::page_faults_now;
using nanobook::latency::PageFaults;

TEST(PrefaultAllocator, EveryPageIsTouchedWhenTheMemoryIsHandedOut) {
    constexpr std::size_t kPage = 4096;
    // Exactly 1 MiB. glibc usually hands out a block this big 16 bytes past a page boundary, so
    // it spans 257 pages, and only the write at its last byte reaches the 257th.
    constexpr std::size_t kLevels = (std::size_t{1} << 20U) / sizeof(Level);
    PrefaultAllocator<Level> allocator;
    Level *memory = allocator.allocate(kLevels);
    const std::size_t size = kLevels * sizeof(Level);

    const PageFaults before = page_faults_now();
    volatile unsigned char *bytes = reinterpret_cast<unsigned char *>(memory); // NOLINT: raw bytes
    for (std::size_t i = 0; i < size; i += kPage) {
        bytes[i] = 1;
    }
    bytes[size - 1] = 1;
    const PageFaults taken = page_faults_now() - before;
    EXPECT_EQ(taken.minor, 0U); // 256 or 257 pages written; all were already mapped
    allocator.deallocate(memory, kLevels);
}

TEST(PrefaultAllocator, AVectorUsingItBehavesLikeAnyVector) {
    std::vector<Level, PrefaultAllocator<Level>> levels;
    levels.reserve(1'000);
    for (int i = 0; i < 10; ++i) {
        levels.insert(levels.begin(), Level{.price = nanobook::Price{i}});
    }
    levels.erase(levels.begin() + 3);
    ASSERT_EQ(levels.size(), 9U);
    EXPECT_EQ(levels.front().price, nanobook::Price{9});
    EXPECT_EQ(levels[3].price, nanobook::Price{5}); // 6 was erased
    EXPECT_EQ(levels.back().price, nanobook::Price{0});
    EXPECT_TRUE(PrefaultAllocator<Level>{} == PrefaultAllocator<int>{}); // stateless
}

/*
 * test_page_faults.cpp: tests for the page-fault counter (support/page_faults.hpp).
 *
 * What it does: Touching fresh memory must count at least one minor fault per 4 KiB page, and no
 *   major fault (nothing is read from disk); touching the same pages again must cost (almost)
 *   nothing; and a fault taken on another thread must be counted too, as the pipeline's three
 *   threads need.
 *
 * Why it exists: Phase 6 decides experiments E2 and E6 partly on these counts (optimise design
 *   §3.2). A counter that missed faults, or counted every access, would decide them wrongly.
 *
 * How it works: The memory comes straight from mmap, so its pages are guaranteed fresh, and is
 *   marked MADV_NOHUGEPAGE so the kernel can't map it as one 2 MiB page with a single fault.
 *   Writes go through a volatile pointer, so the compiler can't skip them.
 *
 * Trade-offs: Only lower bounds are checked for the first touch: the sanitizer builds fault in
 *   their own shadow memory as well, so an exact count would only hold in Release.
 */

#include <gtest/gtest.h>

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <thread>

#include "page_faults.hpp"

using nanobook::latency::page_faults_now;
using nanobook::latency::PageFaults;

namespace {

constexpr std::size_t kPage = 4096;
constexpr std::size_t kPages = 256;

// kPages pages of fresh memory, unmapped again at the end.
class FreshPages {
public:
    FreshPages() {
        void *map = mmap(nullptr, kPages * kPage, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        EXPECT_NE(map, MAP_FAILED);
        madvise(map, kPages * kPage, MADV_NOHUGEPAGE);
        data_ = static_cast<std::byte *>(map);
    }
    ~FreshPages() {
        munmap(data_, kPages * kPage);
    }
    FreshPages(const FreshPages &) = delete;
    FreshPages &operator=(const FreshPages &) = delete;
    FreshPages(FreshPages &&) = delete;
    FreshPages &operator=(FreshPages &&) = delete;

    // Writes one byte in every page.
    void touch() {
        volatile std::byte *p = data_;
        for (std::size_t i = 0; i < kPages; ++i) {
            p[i * kPage] = std::byte{1};
        }
    }

private:
    std::byte *data_{};
};

} // namespace

TEST(PageFaults, EachFreshPageTouchedCountsAMinorFault) {
    FreshPages pages;
    const PageFaults before = page_faults_now();
    pages.touch();
    const PageFaults taken = page_faults_now() - before;
    EXPECT_GE(taken.minor, kPages);
    EXPECT_EQ(taken.major, 0U); // fresh memory is zero-filled, never read from disk
}

TEST(PageFaults, TouchingThePagesAgainCostsNothing) {
    FreshPages pages;
    pages.touch();
    const PageFaults before = page_faults_now();
    pages.touch();
    const PageFaults taken = page_faults_now() - before;
    EXPECT_LT(taken.minor, kPages / 16); // a fault is per first touch, not per access
}

TEST(PageFaults, AFaultOnAnotherThreadIsCounted) {
    FreshPages pages;
    const PageFaults before = page_faults_now();
    std::thread toucher{[&pages] { pages.touch(); }};
    toucher.join();
    const PageFaults taken = page_faults_now() - before;
    EXPECT_GE(taken.minor, kPages); // the whole process, every thread (and ended ones)
}

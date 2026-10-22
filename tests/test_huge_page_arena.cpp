/*
 * test_huge_page_arena.cpp: tests for the books' huge-page arena (support/huge_page_arena.hpp).
 *
 * What it does: The arena hands out memory from its own region, which starts on a 2 MiB boundary
 *   and is the request rounded up to 2 MiB. Asking for more than it holds throws instead of
 *   falling back. DefaultResourceScope sets the default memory resource and puts the old one back.
 *   A FastBook built in the arena takes exactly what arena_bytes() says (one fits, a second does
 *   not), and still works. Every page was touched up front, so writing to it costs no page fault.
 *   And a whole day's books built in the arena end exactly like books built normally (the golden
 *   day and a busy day, compared stock by stock): the timed tools' books are the same books.
 *
 * Why it exists: Optimise design E6. The books' arrays must really land in the arena, and a wrong
 *   size estimate must fail loudly: otherwise the timed runs would quietly use normal pages.
 *
 * Trade-offs: Whether the kernel gives 2 MiB pages depends on the machine (free memory, settings),
 *   so huge_bytes() is only checked for being whole 2 MiB pages within the arena.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <vector>

#include "huge_page_arena.hpp"
#include "itch_golden_day.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "page_faults.hpp"

using nanobook::BookConfig;
using nanobook::FastBook;
using nanobook::support::DefaultResourceScope;
using nanobook::support::HugePageArena;
using nanobook::support::kHugePage;

namespace {

constexpr BookConfig kConfig{.min_price = nanobook::Price{1},
                             .max_price = nanobook::Price{10'000},
                             .max_orders = 10'000,
                             .max_qty = nanobook::Qty{1'000'000}};

} // namespace

TEST(HugePageArena, HandsOutMemoryFromItsOwnAlignedRegion) {
    HugePageArena arena{3 << 20U}; // 3 MiB, rounded up to 4
    EXPECT_EQ(arena.size(), 2 * kHugePage);
    void *first = arena.resource()->allocate(1'000, 8);
    void *second = arena.resource()->allocate(1'000, 8);
    EXPECT_TRUE(arena.contains(first));
    EXPECT_TRUE(arena.contains(second));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first) % kHugePage, 0U); // NOLINT: an address
    EXPECT_EQ(arena.huge_bytes() % kHugePage, 0U);
    EXPECT_LE(arena.huge_bytes(), arena.size());
}

TEST(HugePageArena, AskingForMoreThanItHoldsThrows) {
    HugePageArena arena{kHugePage};
    EXPECT_THROW((void)arena.resource()->allocate(kHugePage + 1, 8), std::bad_alloc);
}

TEST(HugePageArena, TheScopeSetsTheDefaultResourceAndPutsTheOldOneBack) {
    HugePageArena arena{kHugePage};
    std::pmr::memory_resource *before = std::pmr::get_default_resource();
    {
        const DefaultResourceScope scope{arena.resource()};
        EXPECT_EQ(std::pmr::get_default_resource(), arena.resource());
    }
    EXPECT_EQ(std::pmr::get_default_resource(), before);
}

namespace {

// Passes requests on to new/delete and adds up the bytes asked for.
class CountingResource : public std::pmr::memory_resource {
public:
    std::size_t bytes{};

private:
    void *do_allocate(std::size_t n, std::size_t alignment) override {
        bytes += n;
        return std::pmr::new_delete_resource()->allocate(n, alignment);
    }
    void do_deallocate(void *p, std::size_t n, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p, n, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};

} // namespace

TEST(HugePageArena, ArenaBytesIsExactlyWhatABookAsksFor) {
    for (const std::uint32_t max_orders : {1U, 10'000U, 123'457U}) {
        BookConfig config = kConfig;
        config.max_orders = max_orders;
        CountingResource counting;
        {
            const DefaultResourceScope scope{&counting};
            const FastBook book{config};
        }
        EXPECT_EQ(counting.bytes, FastBook::arena_bytes(config)) << max_orders;
    }
}

TEST(HugePageArena, ABookTakesExactlyWhatArenaBytesSays) {
    // One book fits in arena_bytes() plus the alignment allowance, and uses up its share: a second
    // one doesn't fit, so the first really took its arrays from the arena.
    HugePageArena arena{FastBook::arena_bytes(kConfig) + (std::size_t{4} * 64)};
    const std::size_t spare = arena.size() - FastBook::arena_bytes(kConfig); // rounding to 2 MiB
    ASSERT_LT(spare, FastBook::arena_bytes(kConfig)); // so a second book can't fit in the rest
    std::optional<FastBook> book;
    {
        const DefaultResourceScope scope{arena.resource()};
        book.emplace(kConfig);
        EXPECT_THROW(FastBook{kConfig}, std::bad_alloc);
    }
    using nanobook::FeedAdd;
    using nanobook::FeedResult;
    using nanobook::OrderId;
    using nanobook::Side;
    EXPECT_EQ(book->apply(FeedAdd{.id = OrderId{7},
                                  .side = Side::Buy,
                                  .price = nanobook::Price{100},
                                  .qty = nanobook::Qty{10}}),
              FeedResult::Ok);
    EXPECT_EQ(book->best_bid(), nanobook::Price{100});
}

TEST(HugePageArena, EveryPageIsTouchedUpFront) {
    HugePageArena arena{kHugePage};
    auto *bytes = static_cast<unsigned char *>(arena.resource()->allocate(kHugePage, 64));
    const nanobook::latency::PageFaults before = nanobook::latency::page_faults_now();
    volatile unsigned char *page = bytes;
    for (std::size_t i = 0; i < kHugePage; i += 4096) {
        page[i] = 1;
    }
    const nanobook::latency::PageFaults taken = nanobook::latency::page_faults_now() - before;
    EXPECT_EQ(taken.minor, 0U); // 512 pages written; all were mapped when the arena was made
}

TEST(HugePageArena, ADaysBooksBuiltInItEndLikeBooksBuiltNormally) {
    using namespace nanobook::itch;
    using Builder = BookBuilder<FastBook>;
    for (const auto &day : {golden::golden_day(), golden::busy_day(20'000)}) {
        DayCounter counter;
        ASSERT_TRUE(parse(day, counter).has_value());
        const std::vector<BookConfig> configs =
            size_books(counter.directory(), counter.stats(), SizingRules{});

        Builder plain{configs, BuilderOptions{.check = true}};
        HugePageArena arena{nanobook::support::arena_bytes(configs)};
        auto in_arena =
            nanobook::support::build_books<Builder>(arena, configs, BuilderOptions{.check = true});
        ASSERT_TRUE(parse(day, plain).has_value());
        ASSERT_TRUE(parse(day, in_arena).has_value());

        EXPECT_EQ(in_arena.stats().results, plain.stats().results);
        ASSERT_EQ(in_arena.locates(), plain.locates());
        for (std::size_t i = 1; i < plain.locates(); ++i) {
            const auto locate = static_cast<std::uint16_t>(i);
            EXPECT_EQ(compare_books(in_arena.book(locate), plain.book(locate)), std::nullopt)
                << locate;
        }
    }
}

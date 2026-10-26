/*
 * test_l1_table.cpp: tests for the viewer's shared table (support/l1_table.hpp, l1_mapping.hpp).
 *
 * What it does: One thread first: the layout is what viewer design §3.2 says; a quote written is
 *   read back exactly; publish() fills a stock's slot from a TopOfBook and counts its updates; a
 *   slot whose seq is odd (the writer in the middle) is refused; latency lands in the right
 *   bucket and a huge one is clamped into the last; start() and finish() move the state. Then
 *   the seqlock under fire: one thread rewrites a slot with quotes whose fields are all the same
 *   number, while the test reads it as fast as it can; every read accepted must have all fields
 *   equal, and never go back in time. Last, shared memory: a table made by create() is seen by
 *   open(), read-only; a missing or wrong-sized table is refused; and a table written by another
 *   process (fork) is read by this one.
 *
 * Why it exists: A torn read (half one update, half the next) would show a bid from one moment
 *   and an ask from another without any error. The stress test runs under ThreadSanitizer in CI
 *   (rule 14), which reports any access the memory orders don't make safe.
 *
 * How it works: Tables for the one-process tests are on the heap (4.6 MiB is too big for the
 *   stack). Shared-memory names include the process id, so two test runs never collide, and each
 *   test removes its name at the end. Under TSan (5-15x slower) the stress test writes fewer
 *   quotes.
 */

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "l1_mapping.hpp" // support/
#include "l1_table.hpp"

using nanobook::pipeline::TopOfBook;
using nanobook::view::L1Mapping;
using nanobook::view::L1Table;
using nanobook::view::Quote;
using nanobook::view::RunState;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true; // GCC
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true; // Clang
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

constexpr std::uint64_t kStress = kThreadSanitizer ? 100'000 : 5'000'000;

// A name no other test or test run uses.
std::string unique_name(const char *test) {
    return "nanobook_test_" + std::to_string(getpid()) + "_" + test;
}

// Every field the same number: if a read's fields disagree, it was torn.
Quote all_fields(std::uint64_t n) {
    const auto s = static_cast<std::int64_t>(n);
    return Quote{.sequence = n,
                 .timestamp = n,
                 .bid_price = s,
                 .bid_qty = s,
                 .ask_price = s,
                 .ask_qty = s,
                 .updates = n};
}

bool fields_agree(const Quote &q) {
    const auto s = static_cast<std::int64_t>(q.sequence);
    return q.timestamp == q.sequence && q.bid_price == s && q.bid_qty == s && q.ask_price == s &&
           q.ask_qty == s && q.updates == q.sequence;
}

} // namespace

TEST(L1Table, LayoutIsWhatTheDesignSays) {
    static_assert(sizeof(nanobook::view::Slot) == 64);
    EXPECT_EQ(nanobook::view::kLatencyBuckets, 2'432U);
    // The largest 32-bit tick count falls in the last bucket, not past it.
    EXPECT_EQ(nanobook::latency::bucket_of(std::numeric_limits<std::uint32_t>::max()),
              nanobook::view::kLatencyBuckets - 1);
    std::array<char, 8> magic{};
    std::memcpy(magic.data(), &nanobook::view::kMagic, magic.size());
    EXPECT_EQ(std::string(magic.data(), magic.size()), "NBVIEW01");
    EXPECT_LT(sizeof(L1Table), 5U << 20); // about 4.6 MiB
}

TEST(L1Table, AQuoteWrittenIsReadBackExactly) {
    auto table = std::make_unique<L1Table>();
    const Quote q{.sequence = 7,
                  .timestamp = 34'200'000'000'000, // 09:30:00
                  .bid_price = 1'650'500,          // $165.0500
                  .bid_qty = 300,
                  .ask_price = 1'650'600,
                  .ask_qty = 100,
                  .updates = 3};
    nanobook::view::write(table->slots[42], q);
    EXPECT_EQ(nanobook::view::try_read(table->slots[42]), q);
    EXPECT_EQ(table->slots[42].seq.load(), 2U); // two stores per write: odd, then even
    EXPECT_EQ(nanobook::view::try_read(table->slots[43]), Quote{}); // untouched: all zeros
}

TEST(L1Table, PublishFillsTheStocksSlotAndCountsItsUpdates) {
    auto table = std::make_unique<L1Table>();
    const TopOfBook first{.sequence = 1,
                          .timestamp = 100,
                          .bid_price = 500,
                          .bid_qty = 10,
                          .ask_price = 0, // no asks yet
                          .ask_qty = 0,
                          .sent = 0,
                          .locate = 9};
    TopOfBook second = first;
    second.sequence = 2;
    second.timestamp = 200;
    second.ask_price = 510;
    second.ask_qty = 20;
    TopOfBook other = first;
    other.sequence = 3;
    other.timestamp = 300;
    other.locate = 10;

    nanobook::view::publish(*table, first);
    nanobook::view::publish(*table, second);
    nanobook::view::publish(*table, other);

    EXPECT_EQ(nanobook::view::try_read(table->slots[9]), (Quote{.sequence = 2,
                                                                .timestamp = 200,
                                                                .bid_price = 500,
                                                                .bid_qty = 10,
                                                                .ask_price = 510,
                                                                .ask_qty = 20,
                                                                .updates = 2}));
    EXPECT_EQ(nanobook::view::try_read(table->slots[10]).value_or(Quote{}).updates, 1U);
    EXPECT_EQ(table->header.updates.load(), 3U);
    EXPECT_EQ(table->header.itch_time.load(), 300U);
}

TEST(L1Table, ASlotBeingWrittenIsRefused) {
    auto table = std::make_unique<L1Table>();
    nanobook::view::write(table->slots[1], all_fields(5));
    table->slots[1].seq.store(3); // odd: what a reader sees while the writer is in the middle
    EXPECT_FALSE(nanobook::view::try_read(table->slots[1]).has_value());
    table->slots[1].seq.store(4); // finished
    EXPECT_EQ(nanobook::view::try_read(table->slots[1]), all_fields(5));
}

TEST(L1Table, LatencyLandsInItsBucketAndHugeOnesAreClamped) {
    auto table = std::make_unique<L1Table>();
    nanobook::view::record_latency(*table, 500);
    nanobook::view::record_latency(*table, 500);
    nanobook::view::record_latency(*table, 1'000'000);
    nanobook::view::record_latency(*table, std::uint64_t{1} << 40); // ~7 minutes at 2.4 GHz
    EXPECT_EQ(table->latency[500].load(), 2U);
    EXPECT_EQ(table->latency[nanobook::latency::bucket_of(1'000'000)].load(), 1U);
    EXPECT_EQ(table->latency[nanobook::view::kLatencyBuckets - 1].load(), 1U);
}

TEST(L1Table, StartAndFinishMoveTheState) {
    auto table = std::make_unique<L1Table>();
    EXPECT_EQ(nanobook::view::state(*table), RunState::Starting);
    nanobook::view::start(*table, 2'400'000'000);
    EXPECT_EQ(nanobook::view::state(*table), RunState::Running);
    EXPECT_EQ(table->header.ticks_per_second.load(), 2'400'000'000U);
    nanobook::view::finish(*table);
    EXPECT_EQ(nanobook::view::state(*table), RunState::Done);
}

TEST(L1TableThreads, NoReadIsEverTornOrGoesBackInTime) {
    auto table = std::make_unique<L1Table>();
    std::atomic<bool> done{false};
    std::jthread writer{[&] {
        for (std::uint64_t n = 1; n <= kStress; ++n) {
            nanobook::view::write(table->slots[0], all_fields(n));
        }
        done.store(true, std::memory_order_release);
    }};
    std::uint64_t accepted = 0;
    std::uint64_t refused = 0;
    std::uint64_t last = 0;
    bool finished = false;
    while (!finished) {
        finished = done.load(std::memory_order_acquire); // one more read after the last write
        const auto q = nanobook::view::try_read(table->slots[0]);
        if (!q) {
            ++refused;
            continue;
        }
        ++accepted;
        if (!fields_agree(*q)) {
            FAIL() << "torn read: sequence " << q->sequence << ", bid " << q->bid_price;
        }
        if (q->sequence < last) {
            FAIL() << "went back in time: " << q->sequence << " after " << last;
        }
        last = q->sequence;
    }
    writer.join();
    EXPECT_EQ(last, kStress); // the read after the writer finished saw its last quote
    EXPECT_GT(accepted, 0U);
    std::cout << "accepted " << accepted << " reads, refused " << refused << '\n';
}

TEST(L1Mapping, ATableMadeByCreateIsSeenReadOnlyByOpen) {
    const std::string name = unique_name("create_open");
    auto made = L1Mapping::create(name);
    ASSERT_TRUE(made.has_value()) << made.error();
    L1Table *table = made->writable();
    ASSERT_NE(table, nullptr);
    table->symbols[5] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    nanobook::view::write(table->slots[5], all_fields(11));
    nanobook::view::start(*table, 1'000);

    auto seen = L1Mapping::open(name);
    ASSERT_TRUE(seen.has_value()) << seen.error();
    EXPECT_EQ(seen->writable(), nullptr); // a reader can't write
    EXPECT_EQ(nanobook::view::state(seen->table()), RunState::Running);
    EXPECT_EQ(seen->table().symbols[5][0], 'A');
    EXPECT_EQ(nanobook::view::try_read(seen->table().slots[5]), all_fields(11));

    // The same memory, not a copy: a later write shows through the reader's mapping.
    nanobook::view::write(table->slots[5], all_fields(12));
    EXPECT_EQ(nanobook::view::try_read(seen->table().slots[5]), all_fields(12));
    EXPECT_TRUE(L1Mapping::remove(name));
}

TEST(L1Mapping, CreateReplacesAnEarlierTable) {
    const std::string name = unique_name("replace");
    auto first = L1Mapping::create(name);
    ASSERT_TRUE(first.has_value()) << first.error();
    nanobook::view::write(first->writable()->slots[1], all_fields(1));
    auto second = L1Mapping::create(name);
    ASSERT_TRUE(second.has_value()) << second.error();
    auto seen = L1Mapping::open(name);
    ASSERT_TRUE(seen.has_value()) << seen.error();
    EXPECT_EQ(nanobook::view::try_read(seen->table().slots[1]), Quote{}); // the new, empty one
    EXPECT_TRUE(L1Mapping::remove(name));
}

TEST(L1Mapping, AMissingOrWrongSizedTableIsRefused) {
    const std::string name = unique_name("refused");
    EXPECT_FALSE(L1Mapping::open(name).has_value());

    const std::string path = "/" + name;
    const int fd = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 4'096), 0);
    close(fd);
    const auto opened = L1Mapping::open(name);
    ASSERT_FALSE(opened.has_value());
    EXPECT_NE(opened.error().find("not an L1 table"), std::string::npos) << opened.error();
    EXPECT_TRUE(L1Mapping::remove(name));
}

TEST(L1Mapping, ATableWrittenByAnotherProcessIsReadByThisOne) {
    const std::string name = unique_name("fork");
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) { // the writer: make the table, fill it, leave
        auto made = L1Mapping::create(name);
        if (!made) {
            _exit(2);
        }
        L1Table &table = *made->writable();
        table.symbols[3] = {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
        nanobook::view::write(table.slots[3], all_fields(99));
        nanobook::view::finish(table);
        _exit(0); // no destructors, no GoogleTest teardown in the child
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "the child failed";

    auto seen = L1Mapping::open(name); // the writer is gone; the table stays
    ASSERT_TRUE(seen.has_value()) << seen.error();
    EXPECT_EQ(nanobook::view::state(seen->table()), RunState::Done);
    EXPECT_EQ(seen->table().symbols[3][0], 'M');
    EXPECT_EQ(nanobook::view::try_read(seen->table().slots[3]), all_fields(99));
    EXPECT_TRUE(L1Mapping::remove(name));
}

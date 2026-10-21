/*
 * test_itch_timing.cpp: tests for the step 6 timing pieces (support/itch_timing.hpp).
 *
 * What it does: Replaying in windows must give exactly what one parse of the whole buffer gives,
 *   for windows from barely one message up to bigger than the day, including a message cut by a
 *   window's end. A bad message must be reported at its offset in the file, not in the window.
 *   ParseOnly must see every message. TimedOrders must time exactly every Nth order message,
 *   tag each kind correctly, and leave the books exactly as an untimed replay does. The page
 *   faults a replay reports must be the handler's own: none for a parse-only replay although
 *   loading the windows faulted, and every one for a handler that touches a fresh page per
 *   message (phase 6 step 1).
 *
 * Why it exists: The step 6 numbers are only as good as these pieces: a window that dropped or
 *   repeated a message, or a timer that changed what it times, would give wrong numbers with no
 *   error.
 *
 * How it works: The golden day (support/itch_golden_day.hpp) is written to an in-memory file
 *   (memfd_create) and mapped read-only, exactly as nanobook_replay maps the real file, so
 *   releasing pages (MADV_DONTNEED) is safe and tested for real.
 */

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "itch_golden_day.hpp"
#include "itch_replay_comparison.hpp"
#include "itch_timing.hpp"
#include "latency_recorder.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "page_faults.hpp"

using namespace nanobook;
using namespace nanobook::itch;

namespace {

// `bytes` written to an in-memory file and mapped read-only, like nanobook_replay's file.
class MappedCopy {
public:
    explicit MappedCopy(std::span<const std::byte> bytes) : size_(bytes.size()) {
        const int fd = memfd_create("itch_test", 0);
        EXPECT_GE(fd, 0);
        EXPECT_EQ(write(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
        void *map = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        EXPECT_NE(map, MAP_FAILED);
        data_ = static_cast<const std::byte *>(map);
    }
    ~MappedCopy() {
        munmap(const_cast<std::byte *>(data_), size_);
    }
    MappedCopy(const MappedCopy &) = delete;
    MappedCopy &operator=(const MappedCopy &) = delete;
    MappedCopy(MappedCopy &&) = delete;
    MappedCopy &operator=(MappedCopy &&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const {
        return {data_, size_};
    }

private:
    const std::byte *data_{};
    std::size_t size_{};
};

std::vector<BookConfig> configs_for(std::span<const std::byte> day) {
    DayCounter counter;
    EXPECT_TRUE(parse(day, counter).has_value());
    return size_books(counter.directory(), counter.stats(), SizingRules{});
}

// The golden day's books, built by one parse of the whole buffer: the answer to compare with.
BookBuilder<FastBook> replay_whole(std::span<const std::byte> day) {
    BookBuilder<FastBook> builder{configs_for(day), BuilderOptions{.check = true}};
    EXPECT_TRUE(parse(day, builder).has_value());
    return builder;
}

void expect_same(const BookBuilder<FastBook> &a, const BookBuilder<FastBook> &b) {
    EXPECT_EQ(a.stats().messages, b.stats().messages);
    EXPECT_EQ(a.stats().results, b.stats().results);
    EXPECT_EQ(a.stats().crossed_starts, b.stats().crossed_starts);
    ASSERT_EQ(a.locates(), b.locates());
    for (std::size_t i = 1; i < a.locates(); ++i) {
        const auto locate = static_cast<std::uint16_t>(i);
        EXPECT_EQ(compare_books(a.book(locate), b.book(locate)), std::nullopt) << locate;
    }
}

// ASan and TSan keep "shadow" memory beside everything the program reads, and fault on it the
// first time: those faults aren't the handler's, so a check of "no faults" holds in plain builds.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitizer = true;
#else
constexpr bool kSanitizer = false;
#endif
#else
constexpr bool kSanitizer = false;
#endif

constexpr std::uint64_t kGoldenMessages = 31; // 6 S, 3 R, 3 H, 8 adds, 3 E/C, 2 X, 3 D, 1 U, P, I

// A handler that writes to a fresh page of its own for each of its first `pages` messages: one
// page fault each, as a book touching new memory would take.
class TouchesAFreshPagePerMessage {
public:
    explicit TouchesAFreshPagePerMessage(std::size_t pages) : pages_(pages) {
        void *map = mmap(nullptr, pages * kPage, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        EXPECT_NE(map, MAP_FAILED);
        madvise(map, pages * kPage, MADV_NOHUGEPAGE); // one fault per 4 KiB page, not per 2 MiB
        memory_ = static_cast<std::byte *>(map);
    }
    ~TouchesAFreshPagePerMessage() {
        munmap(memory_, pages_ * kPage);
    }
    TouchesAFreshPagePerMessage(const TouchesAFreshPagePerMessage &) = delete;
    TouchesAFreshPagePerMessage &operator=(const TouchesAFreshPagePerMessage &) = delete;
    TouchesAFreshPagePerMessage(TouchesAFreshPagePerMessage &&) = delete;
    TouchesAFreshPagePerMessage &operator=(TouchesAFreshPagePerMessage &&) = delete;

    template <typename Message> void on(const Message & /*message*/) {
        touch();
    }
    void on_skipped(char /*type*/) {
        touch();
    }
    [[nodiscard]] std::size_t touched() const {
        return touched_;
    }

private:
    void touch() {
        if (touched_ < pages_) {
            volatile std::byte *p = memory_; // volatile: the write can't be skipped
            p[touched_ * kPage] = std::byte{1};
            ++touched_;
        }
    }

    std::byte *memory_{};
    std::size_t pages_;
    std::size_t touched_{};
};

} // namespace

TEST(ItchWindows, GiveExactlyWhatOneParseGives) {
    const std::vector<std::byte> day = golden::golden_day();
    const MappedCopy file{day};
    const BookBuilder<FastBook> whole = replay_whole(file.bytes());

    // 52 bytes: the longest message (50) plus its length. 100: cuts messages in two often.
    for (const std::size_t window :
         {std::size_t{52}, std::size_t{100}, std::size_t{4096}, day.size(), day.size() * 2}) {
        SCOPED_TRACE(window);
        BookBuilder<FastBook> windowed{configs_for(day), BuilderOptions{.check = true}};
        const auto run = replay_in_windows(file.bytes(), window, windowed);
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->bytes, day.size());
        EXPECT_GE(run->windows, (day.size() + window - 1) / window);
        expect_same(whole, windowed);
    }
}

TEST(ItchWindows, ReportABadMessageAtItsOffsetInTheFile) {
    std::vector<std::byte> day = golden::golden_day();
    Writer bad;
    bad.malformed(19, 'Z');
    day.insert(day.end(), bad.bytes().begin(), bad.bytes().end());
    const MappedCopy file{day};
    ParseOnly handler;
    const auto run = replay_in_windows(file.bytes(), 100, handler);
    ASSERT_FALSE(run.has_value());
    EXPECT_EQ(run.error().offset, day.size() - 21); // where the bad message's length starts
    EXPECT_EQ(run.error().type, 'Z');
}

TEST(ItchWindows, ParseOnlySeesEveryMessage) {
    const std::vector<std::byte> day = golden::golden_day();
    const MappedCopy file{day};
    ParseOnly handler;
    ASSERT_TRUE(replay_in_windows(file.bytes(), 64, handler).has_value());
    EXPECT_EQ(handler.messages, kGoldenMessages);
    EXPECT_NE(handler.sum, 0U);
}

TEST(ItchWindows, CountOnlyThePageFaultsTakenWhileTimed) {
    const std::vector<std::byte> day = golden::busy_day(5'000); // a few hundred KB
    const MappedCopy file{day};
    constexpr std::size_t kWindow = std::size_t{64} * 1024;

    // Parsing alone touches no new memory: loading the windows faults, but not while timed.
    ParseOnly parse_only;
    const latency::PageFaults before = latency::page_faults_now();
    const auto quiet = replay_in_windows(file.bytes(), kWindow, parse_only);
    const latency::PageFaults whole = latency::page_faults_now() - before;
    ASSERT_TRUE(quiet.has_value());
    EXPECT_GT(quiet->windows, 2U);
    EXPECT_GE(whole.minor, quiet->windows); // each window's loading faulted at least once...
    if (!kSanitizer) {
        EXPECT_LT(quiet->faults.minor, quiet->windows); // ...outside the count (Release: 0)
    }

    // A handler that touches a fresh page per message: every one of those faults is counted.
    TouchesAFreshPagePerMessage toucher{1'000};
    const auto busy = replay_in_windows(file.bytes(), kWindow, toucher);
    ASSERT_TRUE(busy.has_value());
    ASSERT_EQ(toucher.touched(), 1'000U);
    EXPECT_GE(busy->faults.minor, 1'000U);
    EXPECT_EQ(busy->faults.major, 0U);
}

TEST(TimedOrders, TimesEveryOrderMessageAndTagsItsKind) {
    const std::vector<std::byte> day = golden::golden_day();
    const MappedCopy file{day};
    latency::Recorder recorder{100};
    BookBuilder<FastBook> builder{configs_for(day), BuilderOptions{.check = true}};
    TimedOrders timed{builder, recorder, 1};
    ASSERT_TRUE(replay_in_windows(file.bytes(), 64, timed).has_value());

    // All 17 order messages (the one for locate 9 too: it costs time even with no book).
    ASSERT_EQ(recorder.size() + recorder.dropped(), 17U);
    const auto count = [&](latency::Tag tag) { return std::ranges::count(recorder.tags(), tag); };
    if (recorder.dropped() == 0) { // a move to another core mid-sample drops it: rare, allowed
        EXPECT_EQ(count(latency::Tag::Add), 8);     // 7 A + 1 F
        EXPECT_EQ(count(latency::Tag::Match), 3);   // 2 E + 1 C
        EXPECT_EQ(count(latency::Tag::Modify), 2);  // 2 X
        EXPECT_EQ(count(latency::Tag::Cancel), 3);  // 3 D
        EXPECT_EQ(count(latency::Tag::Replace), 1); // 1 U
    }
    expect_same(replay_whole(file.bytes()), builder); // timing changed nothing
}

TEST(TimedOrders, TimesOnlyEveryNth) {
    const std::vector<std::byte> day = golden::golden_day();
    const MappedCopy file{day};
    latency::Recorder recorder{100};
    BookBuilder<FastBook> builder{configs_for(day), BuilderOptions{.check = true}};
    TimedOrders timed{builder, recorder, 4};
    ASSERT_TRUE(replay_in_windows(file.bytes(), 4096, timed).has_value());
    EXPECT_EQ(recorder.size() + recorder.dropped(), 17U / 4); // the 4th, 8th, 12th and 16th
    expect_same(replay_whole(file.bytes()), builder);
}

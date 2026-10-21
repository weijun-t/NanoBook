/*
 * test_binary_log.cpp: tests for the publisher's binary log (support/binary_log.hpp).
 *
 * What it does: A log reads back exactly what was written; leaves out the time stamp (which
 *   differs every run); gives the same file with a 100-byte buffer (a write() for almost every
 *   record) as with 1 MiB; its checksum equals an FNV-1a hash of the file computed separately;
 *   an unwritable path is reported, not thrown; a log cut mid-record is rejected. Then the step 3
 *   proof: the golden day and a busy day, on one thread and through the three-thread pipeline,
 *   give byte-identical log files.
 *
 * Why it exists: Rule 11 (determinism) is how phase 5 proves the threads changed nothing, and it
 *   only holds if the same updates always give the same bytes: no padding, no time stamps.
 *
 * How it works: Logs go to files in the system's temporary folder, compared byte by byte, and are
 *   removed at the end of each test.
 */

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "binary_log.hpp"
#include "itch_golden_day.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/pipeline/stages.hpp"

using namespace nanobook;
using namespace nanobook::pipeline;

namespace {

// A file in the temporary folder, deleted when the test ends.
class TempFile {
public:
    explicit TempFile(const std::string &name)
        : path_((std::filesystem::temp_directory_path() /
                 ("nanobook_" + std::to_string(::getpid()) + "_" + name))
                    .string()) {
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    TempFile(TempFile &&) = delete;
    TempFile &operator=(TempFile &&) = delete;

    [[nodiscard]] const std::string &path() const {
        return path_;
    }

private:
    std::string path_;
};

std::vector<char> file_bytes(const std::string &path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

TopOfBook update(std::uint64_t n) {
    return TopOfBook{.sequence = n,
                     .timestamp = 1'000 + n,
                     .bid_price = 1'650'000 + static_cast<std::int64_t>(n),
                     .bid_qty = 100,
                     .ask_price = 1'650'300,
                     .ask_qty = static_cast<std::int64_t>(n),
                     .sent = 987'654'321 + n, // must not reach the log
                     .locate = 14};
}

using Builder = itch::BookBuilder<FastBook>;

std::vector<BookConfig> configs_for(std::span<const std::byte> day) {
    itch::DayCounter counter;
    EXPECT_TRUE(itch::parse(day, counter).has_value());
    return itch::size_books(counter.directory(), counter.stats(), itch::SizingRules{});
}

auto whole(std::span<const std::byte> day) {
    return [day](auto &handler) { return itch::parse(day, handler).has_value(); };
}

// One thread and three threads, each into its own log file: the files must be identical.
void expect_identical_logs(std::span<const std::byte> day, const std::string &name) {
    const TempFile one{name + "_one.log"};
    const TempFile three{name + "_three.log"};
    std::uint64_t one_checksum = 0;
    {
        Builder builder{configs_for(day), itch::BuilderOptions{}};
        BinaryLog log{one.path(), true};
        EXPECT_TRUE(run_sequential(whole(day), builder, log).parsed);
        log.close();
        EXPECT_TRUE(log.ok());
        one_checksum = log.checksum();
    }
    {
        Builder builder{configs_for(day), itch::BuilderOptions{}};
        BinaryLog log{three.path(), true};
        EXPECT_TRUE(run_pipeline(whole(day), builder, log, PipelineOptions{}).parsed);
        log.close();
        EXPECT_TRUE(log.ok());
        EXPECT_EQ(log.checksum(), one_checksum);
    }
    const std::vector<char> a = file_bytes(one.path());
    const std::vector<char> b = file_bytes(three.path());
    EXPECT_GT(a.size(), kLogHeader);
    EXPECT_TRUE(a == b) << "the logs differ: " << a.size() << " vs " << b.size() << " bytes";
}

} // namespace

TEST(BinaryLog, ReadsBackWhatWasWrittenWithoutTheTimeStamp) {
    const TempFile f{"roundtrip.log"};
    {
        BinaryLog log{f.path()};
        for (std::uint64_t n = 1; n <= 3; ++n) {
            log.append(update(n));
        }
        EXPECT_EQ(log.records(), 3U);
        EXPECT_EQ(log.bytes(), kLogHeader + 3 * sizeof(LogRecord));
    } // the destructor flushes and closes
    // An unreadable log gives an empty list, which fails the size check below.
    const std::vector<LogRecord> records = read_log(f.path()).value_or(std::vector<LogRecord>{});
    ASSERT_EQ(records.size(), 3U);
    for (std::uint64_t n = 1; n <= 3; ++n) {
        // Every field against the update itself, not against to_log_record() (the code under
        // test): a field it forgot would otherwise be forgotten on both sides.
        const LogRecord &r = records[n - 1];
        const TopOfBook u = update(n);
        EXPECT_EQ(r.sequence, u.sequence);
        EXPECT_EQ(r.timestamp, u.timestamp);
        EXPECT_EQ(r.bid_price, u.bid_price);
        EXPECT_EQ(r.bid_qty, u.bid_qty);
        EXPECT_EQ(r.ask_price, u.ask_price);
        EXPECT_EQ(r.ask_qty, u.ask_qty);
        EXPECT_EQ(r.locate, u.locate);
        EXPECT_EQ(r.reserved16, 0U);
        EXPECT_EQ(r.reserved32, 0U);
    }
    // `sent` differs every run, so it is not in the record at all: two updates that differ
    // only in `sent` give the same bytes.
    TopOfBook later = update(1);
    later.sent += 12'345;
    EXPECT_EQ(to_log_record(later), to_log_record(update(1)));
}

TEST(BinaryLog, ATinyBufferWritesTheSameFileAsABigOne) {
    const TempFile small{"small_buffer.log"};
    const TempFile big{"big_buffer.log"};
    std::uint64_t small_checksum = 0;
    std::uint64_t big_checksum = 0;
    {
        BinaryLog log{small.path(), true, 100}; // a write() for almost every record
        BinaryLog log2{big.path(), true};       // 1 MiB: one write() at the end
        for (std::uint64_t n = 1; n <= 1'000; ++n) {
            log.append(update(n));
            log2.append(update(n));
        }
        log.close();
        log2.close();
        small_checksum = log.checksum();
        big_checksum = log2.checksum();
    }
    EXPECT_EQ(file_bytes(small.path()), file_bytes(big.path()));
    EXPECT_EQ(small_checksum, big_checksum);
}

TEST(BinaryLog, ItsChecksumIsAnFnvHashOfTheFile) {
    const TempFile f{"checksum.log"};
    BinaryLog log{f.path(), true, 256};
    for (std::uint64_t n = 1; n <= 100; ++n) {
        log.append(update(n));
    }
    log.close();
    std::uint64_t hash = 14'695'981'039'346'656'037ULL; // FNV-1a, computed here independently
    for (const char c : file_bytes(f.path())) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1'099'511'628'211ULL;
    }
    EXPECT_EQ(log.checksum(), hash);
}

TEST(BinaryLog, AnUnwritablePathIsReportedNotThrown) {
    BinaryLog log{"/nonexistent-folder/nanobook.log"};
    EXPECT_FALSE(log.ok());
    log.append(update(1)); // harmless: nothing is written
    log.close();
    EXPECT_FALSE(log.ok());
}

TEST(BinaryLog, ALogCutMidRecordIsRejected) {
    const TempFile f{"cut.log"};
    {
        BinaryLog log{f.path()};
        log.append(update(1));
        log.append(update(2));
    }
    std::filesystem::resize_file(f.path(), kLogHeader + sizeof(LogRecord) + 10);
    EXPECT_FALSE(read_log(f.path()).has_value());
    const TempFile not_a_log{"not_a_log.log"};
    std::ofstream{not_a_log.path()} << "hello, this is not a log at all";
    EXPECT_FALSE(read_log(not_a_log.path()).has_value());
}

TEST(BinaryLog, OneThreadAndThePipelineWriteByteIdenticalLogs) {
    expect_identical_logs(itch::golden::golden_day(), "golden");
    expect_identical_logs(itch::golden::busy_day(20'000), "busy");
}

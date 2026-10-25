/*
 * binary_log.hpp: the publisher's log: every TopOfBook update written to a file as raw bytes.
 *
 * What it does: BinaryLog writes a 16-byte header ("NBLOG001", the record size), then one
 *   56-byte LogRecord per update. append() copies a record into a 1 MiB buffer; when the buffer
 *   is full, one write() system call sends it all to the file. It can also keep a checksum of
 *   every byte it writes, so two runs can be proved identical without keeping their files.
 *   read_log() reads a log back (nanobook_logdump, the tests).
 *
 * Why it exists: plan rule 7: logging is asynchronous and binary. The engine thread never
 *   touches the file: the publisher thread does (pipeline design §6), and turning numbers into
 *   text happens afterwards, in nanobook_logdump. Rule 11: the same day must give a byte-identical
 *   log however the threads were scheduled, which is how step 3 proves the pipeline changed
 *   nothing.
 *
 * How it works:
 *   - LogRecord holds only what comes from the input (no time-stamp counter, which differs every
 *     run), and has no padding: every one of its 56 bytes is a field, set on every write, so the
 *     same update always gives the same bytes. static_assert(has_unique_object_representations)
 *     proves there are no padding bytes.
 *   - append(): one 56-byte copy, O(1); no allocation (the buffer is made in the constructor).
 *   - flush(): write() until every byte is out (a write may take only part, or be interrupted
 *     by a signal), then reuse the buffer. About 18,700 records per write() for 1 MiB.
 *   - The checksum is FNV-1a over the bytes, computed when a buffer is flushed (cold-ish: the
 *     publisher's time, never the engine's).
 *
 * Data structures: one std::vector<std::byte> buffer, one file descriptor.
 *
 * Trade-offs: Linux only (open, write, close). A record is a little larger than its data (the
 *   header and the reserved field) but fixed-size, so a reader can jump to record n directly.
 *   Errors (disk full, unwritable path) stop further writes and are reported by ok(), never by
 *   an exception on the publisher thread.
 */

#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "nanobook/pipeline/records.hpp"

namespace nanobook::pipeline {

struct LogRecord {
    std::uint64_t sequence{};
    std::uint64_t timestamp{}; // ITCH: nanoseconds since midnight
    std::int64_t bid_price{};  // 1/10,000 of a dollar; 0 when the side is empty
    std::int64_t bid_qty{};
    std::int64_t ask_price{};
    std::int64_t ask_qty{};
    std::uint16_t locate{};
    std::uint16_t reserved16{}; // always 0: written out so the record has no padding
    std::uint32_t reserved32{};

    bool operator==(const LogRecord &) const = default;
};
static_assert(sizeof(LogRecord) == 56);
static_assert(std::has_unique_object_representations_v<LogRecord>,
              "a padding byte would make two identical updates give different bytes");

inline LogRecord to_log_record(const TopOfBook &t) {
    return LogRecord{.sequence = t.sequence,
                     .timestamp = t.timestamp,
                     .bid_price = t.bid_price,
                     .bid_qty = t.bid_qty,
                     .ask_price = t.ask_price,
                     .ask_qty = t.ask_qty,
                     .locate = t.locate};
}

inline constexpr std::array<char, 8> kLogMagic{'N', 'B', 'L', 'O', 'G', '0', '0', '1'};
inline constexpr std::size_t kLogHeader = 16; // magic (8), record size (4), reserved (4)

class BinaryLog {
public:
    // Opens (or replaces) `path` and writes the header. `checksum`: also hash every byte.
    explicit BinaryLog(const std::string &path, bool checksum = false,
                       std::size_t buffer_bytes = std::size_t{1} << 20U)
        : buffer_(buffer_bytes), checksum_on_(checksum) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ok_ = fd_ >= 0 && buffer_bytes >= sizeof(LogRecord);
        std::array<std::byte, kLogHeader> header{};
        std::memcpy(header.data(), kLogMagic.data(), kLogMagic.size());
        const auto size = static_cast<std::uint32_t>(sizeof(LogRecord));
        std::memcpy(header.data() + 8, &size, sizeof(size));
        append_bytes(header.data(), header.size());
    }
    BinaryLog(const BinaryLog &) = delete;
    BinaryLog &operator=(const BinaryLog &) = delete;
    BinaryLog(BinaryLog &&) = delete;
    BinaryLog &operator=(BinaryLog &&) = delete;
    ~BinaryLog() {
        close();
    }

    // The publisher's sink: one record per update.
    void operator()(const TopOfBook &t) {
        append(t);
    }
    void append(const TopOfBook &t) {
        const LogRecord r = to_log_record(t);
        append_bytes(reinterpret_cast<const std::byte *>(&r), sizeof(r)); // NOLINT: bytes of r
        ++records_;
    }

    // Writes what is buffered and closes the file. Safe to call twice.
    void close() {
        flush();
        if (fd_ >= 0) {
            ok_ = (::close(fd_) == 0) && ok_;
            fd_ = -1;
        }
    }

    [[nodiscard]] bool ok() const {
        return ok_;
    }
    [[nodiscard]] std::uint64_t records() const {
        return records_;
    }
    [[nodiscard]] std::uint64_t bytes() const {
        return written_ + used_;
    }
    // FNV-1a over every byte written so far, header included (only if `checksum` was asked).
    [[nodiscard]] std::uint64_t checksum() const {
        return hash_;
    }

private:
    void append_bytes(const std::byte *data, std::size_t n) {
        if (used_ + n > buffer_.size()) {
            flush();
        }
        std::memcpy(buffer_.data() + used_, data, n);
        used_ += n;
    }

    void flush() {
        if (checksum_on_) {
            for (std::size_t i = 0; i < used_; ++i) {
                hash_ = (hash_ ^ std::to_integer<std::uint64_t>(buffer_[i])) * kFnvPrime;
            }
        }
        std::size_t done = 0;
        while (ok_ && done < used_) {
            const ssize_t n = ::write(fd_, buffer_.data() + done, used_ - done);
            if (n < 0 && errno == EINTR) {
                continue; // interrupted by a signal before writing anything: try again
            }
            if (n <= 0) {
                ok_ = false; // disk full, or the file can't be written
                break;
            }
            done += static_cast<std::size_t>(n); // a write may take only part of the buffer
        }
        written_ += used_;
        used_ = 0;
    }

    static constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
    static constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

    std::vector<std::byte> buffer_;
    std::size_t used_{0};
    std::uint64_t written_{0};
    std::uint64_t records_{0};
    std::uint64_t hash_{kFnvOffset};
    int fd_{-1};
    bool ok_{false};
    bool checksum_on_{false};
};

// Reads a whole log back: nothing if the file is missing, has the wrong header, or ends in the
// middle of a record. For tools and tests (it allocates).
inline std::optional<std::vector<LogRecord>> read_log(const std::string &path) {
    std::ifstream in{path, std::ios::binary};
    std::array<char, kLogHeader> header{};
    if (!in.read(header.data(), header.size())) {
        return std::nullopt;
    }
    std::uint32_t size = 0;
    std::memcpy(&size, header.data() + 8, sizeof(size));
    if (std::memcmp(header.data(), kLogMagic.data(), kLogMagic.size()) != 0 ||
        size != sizeof(LogRecord)) {
        return std::nullopt;
    }
    std::vector<LogRecord> records;
    LogRecord r;
    while (in.read(reinterpret_cast<char *>(&r), sizeof(r))) { // NOLINT: bytes of r
        records.push_back(r);
    }
    if (in.gcount() != 0) {
        return std::nullopt; // a partial record at the end
    }
    return records;
}

} // namespace nanobook::pipeline

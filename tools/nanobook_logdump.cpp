/*
 * nanobook_logdump.cpp: turns a pipeline binary log into CSV text.
 *
 * What it does: Reads a log written by the pipeline's publisher (support/binary_log.hpp), checks
 *   its header, and prints one CSV line per TopOfBook update: sequence, ITCH time of day, locate,
 *   best bid price and size, best ask price and size (prices in dollars; an empty side prints 0).
 *   --head N stops after N records. Exit code 0 if the whole log was read, 1 if it is missing,
 *   not a NanoBook log, or ends in the middle of a record; 2 bad arguments.
 *     nanobook_logdump LOG [--head N]
 *
 * Why it exists: README rule 7: the hot path writes fixed-size binary records and something else
 *   formats them. Formatting text at millions of records per second would slow the publisher,
 *   so it happens here, afterwards, only when someone wants to read the log.
 *
 * How it works: Streams the file one 56-byte record at a time, so a 20 GB log needs no more
 *   memory than a small one. O(records).
 *
 * Trade-offs: A tool: iostream and snprintf are fine here.
 */

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include "binary_log.hpp" // support/

namespace {

using nanobook::pipeline::kLogHeader;
using nanobook::pipeline::kLogMagic;
using nanobook::pipeline::LogRecord;

constexpr std::string_view kUsage = "usage: nanobook_logdump LOG [--head N]\n";

std::string dollars(std::int64_t ticks) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%lld.%04lld", static_cast<long long>(ticks / 10'000),
                  static_cast<long long>(ticks % 10'000));
    return text.data();
}

std::string time_of_day(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000;
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60),
                  static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000));
    return text.data();
}

} // namespace

int main(int argc, char **argv) {
    std::string path;
    std::uint64_t head = std::numeric_limits<std::uint64_t>::max();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--head" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), head);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                std::cerr << kUsage;
                return 2;
            }
        } else if (path.empty() && !arg.starts_with("--")) {
            path = arg;
        } else {
            std::cerr << kUsage;
            return 2;
        }
    }
    if (path.empty()) {
        std::cerr << kUsage;
        return 2;
    }

    std::ifstream in{path, std::ios::binary};
    std::array<char, kLogHeader> header{};
    std::uint32_t size = 0;
    if (in.read(header.data(), header.size())) {
        std::memcpy(&size, header.data() + 8, sizeof(size));
    }
    if (!in || std::memcmp(header.data(), kLogMagic.data(), kLogMagic.size()) != 0 ||
        size != sizeof(LogRecord)) {
        std::cerr << path << ": not a NanoBook pipeline log\n";
        return 1;
    }

    std::cout << "sequence,time,locate,bid_price,bid_qty,ask_price,ask_qty\n";
    LogRecord r;
    for (std::uint64_t n = 0;
         n < head && in.read(reinterpret_cast<char *>(&r), sizeof(r)); // NOLINT
         ++n) {
        std::cout << r.sequence << "," << time_of_day(r.timestamp) << "," << r.locate << ","
                  << dollars(r.bid_price) << "," << r.bid_qty << "," << dollars(r.ask_price) << ","
                  << r.ask_qty << "\n";
    }
    if (in.gcount() != 0 && in.gcount() != static_cast<std::streamsize>(sizeof(r))) {
        std::cerr << path << ": ends in the middle of a record\n";
        return 1;
    }
    return 0;
}

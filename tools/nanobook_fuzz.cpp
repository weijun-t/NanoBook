/*
 * nanobook_fuzz.cpp: the differential fuzzer as a command-line program.
 *
 * What it does: Runs support/differential_fuzz.hpp for one or more seeds and prints PASS with the
 *   speed, or the first mismatch with the command that replays it. Exit code 0 means every seed
 *   passed, 1 means a mismatch, 2 means bad arguments.
 *     nanobook_fuzz [--ops N] [--seed S] [--seeds K] [--max-orders M]
 *                   [--state-every N] [--invariants-every N]
 *
 * Why it exists: CI runs a short fuzz on every push. Phase 2's exit check (10 million or more
 *   operations on several seeds) runs here, locally.
 *
 * How it works: Book A is the reference book. Book B is the book being checked: also the
 *   reference book today (proving the fuzzer never reports a false difference), the fast book from
 *   phase 2 step 4. Seeds S, S+1, ..., S+K-1 run one after another.
 *
 * Trade-offs: A tool, not the engine: it prints with iostream and measures time with a clock,
 *   both of which the hot path must never do (rule 6).
 */

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "differential_fuzz.hpp"
#include "nanobook/reference/reference_book.hpp"

namespace {

using BookA = nanobook::ReferenceBook; // the trusted answer key
using BookB = nanobook::ReferenceBook; // phase 2 step 4: nanobook::FastBook

constexpr std::string_view kUsage =
    "usage: nanobook_fuzz [--ops N] [--seed S] [--seeds K] [--max-orders M]\n"
    "                     [--state-every N] [--invariants-every N]\n";

// Reads a whole decimal number; false if `text` isn't one.
template <typename T> bool parse(std::string_view text, T &value) {
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

} // namespace

int main(int argc, char **argv) {
    nanobook::fuzz::Options options;
    std::uint64_t seeds = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << flag << "\n" << kUsage;
            return 2;
        }
        const std::string_view value = argv[++i];
        bool ok = false;
        if (flag == "--ops") {
            ok = parse(value, options.ops);
        } else if (flag == "--seed") {
            ok = parse(value, options.seed);
        } else if (flag == "--seeds") {
            ok = parse(value, seeds);
        } else if (flag == "--max-orders") {
            ok = parse(value, options.max_orders);
        } else if (flag == "--state-every") {
            ok = parse(value, options.state_every) && options.state_every > 0;
        } else if (flag == "--invariants-every") {
            ok = parse(value, options.invariants_every) && options.invariants_every > 0;
        }
        if (!ok) {
            std::cerr << "bad argument: " << flag << " " << value << "\n" << kUsage;
            return 2;
        }
    }

    const std::uint64_t first_seed = options.seed;
    for (std::uint64_t s = first_seed; s < first_seed + seeds; ++s) {
        options.seed = s;
        const auto start = std::chrono::steady_clock::now();
        const auto mismatch = nanobook::fuzz::run<BookA, BookB>(options);
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;

        if (mismatch) {
            std::cout << nanobook::fuzz::report(*mismatch);
            return 1;
        }
        std::cout << "seed " << s << ": PASS, " << options.ops << " ops in " << took.count()
                  << " s (" << static_cast<double>(options.ops) / took.count() / 1e6
                  << " M ops/s)\n";
    }
    return 0;
}

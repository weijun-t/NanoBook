/*
 * fuzz_itch.cpp: a libFuzzer target: random bytes through the ITCH parser into two book builders.
 *
 * What it does: libFuzzer calls LLVMFuzzerTestOneInput() with millions of byte strings. The first
 *   byte picks a mode. Raw (even): the rest is parsed as an ITCH file as it is. Structured (odd):
 *   the rest is read as a list of operations, and the test encoder writes them as valid ITCH.
 *   Either way, every message goes to two BookBuilders at once, one over the reference book and
 *   one over the fast book (ReplayComparison). The run fails (and libFuzzer
 *   saves the input) if anything crashes, if ASan or UBSan sees a memory error or undefined
 *   behaviour, if the parser claims more bytes than it was given, if a book's structure breaks,
 *   or if the two builders ever disagree.
 *     cmake --workflow --preset clang-fuzz        (builds it)
 *     build/clang-fuzz/nanobook_fuzz_itch -max_total_time=60 CORPUS_DIR
 *
 * Why it exists: README §7.1 and polish design Q6. A feed handler reads bytes it doesn't control:
 *   a damaged file, a bug upstream. It must reject bad input, never crash. The unit tests check
 *   the bad inputs someone thought of; a fuzzer finds the ones nobody did. Parsing alone is a
 *   small target (every length is checked first), so the bytes go on into the books, where
 *   messages with valid lengths but nonsense fields (unknown stocks, deleting orders that never
 *   existed, prices off the band, a full book) test the builders and both books.
 *
 * How it works: libFuzzer instruments every branch and comparison (-fsanitize=fuzzer) and keeps
 *   any input that reaches new code, then mutates those. Raw mode tests the parser's checks. But
 *   random bytes almost never make a valid Stock Directory followed by an add for the same stock
 *   (a 60 s raw-only trial reached 179 branches and never a book), hence the structured mode:
 *   each 5 bytes are one operation (type, stock, order id, shares, price), each drawn from a
 *   small range, so operations keep landing on the same few orders: a delete finds an order an
 *   add made, a replace moves it, an execute takes more shares than it has. Per input:
 *   two builders of 3 small books each (constructing them allocates, which is fine here), one
 *   parse, then a full comparison after every order message, a structure check after every order
 *   message, and the final statistics comparison. O(input bytes x orders per book).
 *
 * Data structures: The 3 books per builder (locate 0 is never a stock) are deliberately tiny, 16
 *   orders and prices 1 to 1,000, so the fuzzer reaches "book full" and "price off the band"
 *   within a few messages. Structured mode: stocks 0 to 4 (0 and 4 have no book), order ids 0 to
 *   24 (0 is "no id"), shares 0 to 255, prices 0 to 1,275 (past the band).
 *
 * Trade-offs: Clang only (libFuzzer is part of Clang). Small books find edge cases quickly but
 *   never exercise the sizes of a real day; the full-day differential replay covers those.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <print>
#include <span>
#include <vector>

#include "itch_encoder.hpp"
#include "itch_replay_comparison.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/reference/reference_book.hpp"

using namespace nanobook;
using namespace nanobook::itch;

namespace {

const std::vector<BookConfig> kConfigs(4, BookConfig{.min_price = Price{1},
                                                     .max_price = Price{1000},
                                                     .max_orders = 16,
                                                     .max_qty = Qty{1'000'000}});

constexpr std::array<Symbol, 5> kStocks{
    Symbol{'Z', 'E', 'R', 'O', ' ', ' ', ' ', ' '}, Symbol{'A', 'A', 'A', ' ', ' ', ' ', ' ', ' '},
    Symbol{'B', 'B', 'B', ' ', ' ', ' ', ' ', ' '}, Symbol{'C', 'C', 'C', ' ', ' ', ' ', ' ', ' '},
    Symbol{'D', 'D', 'D', ' ', ' ', ' ', ' ', ' '}};

// Structured mode: every 5 bytes become one valid ITCH message. Stocks 1 and 2 are named up
// front; stock 3 only if an operation names it.
std::vector<std::byte> encode_operations(std::span<const std::uint8_t> ops) {
    Writer w;
    std::uint64_t time = 1;
    w.stock_directory({.locate = 1, .timestamp = time++, .stock = kStocks[1]});
    w.stock_directory({.locate = 2, .timestamp = time++, .stock = kStocks[2]});
    for (std::size_t i = 0; i + 5 <= ops.size(); i += 5) {
        const auto locate = static_cast<std::uint16_t>(ops[i + 1] % 5);
        const std::uint64_t ref = ops[i + 2] % 25;
        const std::uint32_t shares = ops[i + 3];
        const auto price = static_cast<std::uint32_t>(ops[i + 4]) * 5;
        const std::uint64_t t = time++;
        switch (ops[i] % 11) {
        case 0:
            w.stock_directory({.locate = locate, .timestamp = t, .stock = kStocks[locate]});
            break;
        case 1:
        case 2: { // adds twice as often, so books fill up
            const AddOrder a{.locate = locate,
                             .timestamp = t,
                             .order_ref = ref,
                             .side = (ops[i] & 0x10) != 0 ? 'S' : 'B',
                             .shares = shares,
                             .stock = kStocks[locate],
                             .price = price};
            if ((ops[i] & 0x20) != 0) {
                w.add_order_mpid(a);
            } else {
                w.add_order(a);
            }
            break;
        }
        case 3:
            w.executed(
                {.locate = locate, .timestamp = t, .order_ref = ref, .executed_shares = shares});
            break;
        case 4:
            w.executed_with_price({.locate = locate,
                                   .timestamp = t,
                                   .order_ref = ref,
                                   .executed_shares = shares,
                                   .printable = 'Y',
                                   .execution_price = price});
            break;
        case 5:
            w.cancel(
                {.locate = locate, .timestamp = t, .order_ref = ref, .cancelled_shares = shares});
            break;
        case 6:
            w.delete_order({.locate = locate, .timestamp = t, .order_ref = ref});
            break;
        case 7:
            w.replace({.locate = locate,
                       .timestamp = t,
                       .original_order_ref = ref,
                       .new_order_ref = std::uint64_t{ops[i]} % 25,
                       .shares = shares,
                       .price = price});
            break;
        case 8: // trading state: H halted, P paused, Q quotation only, T trading
            w.trading_action({.locate = locate,
                              .timestamp = t,
                              .stock = kStocks[locate],
                              .trading_state = "HPQT"[ops[i + 3] % 4]});
            break;
        case 9: // system event: start of day, market open/close, ... (a full comparison of all)
            w.system_event({.timestamp = t, .event_code = "OSQMEC"[ops[i + 3] % 6]});
            break;
        default:
            w.skipped('P'); // a trade message: NanoBook skips it
            break;
        }
    }
    const auto bytes = w.bytes();
    return {bytes.begin(), bytes.end()};
}

[[noreturn]] void fail(const char *what) {
    std::println(stderr, "fuzz_itch: {}", what);
    __builtin_trap(); // libFuzzer reports the crash and saves the input
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const BuilderOptions options{.check = true, .structure_every = 1};
    BookBuilder<ReferenceBook> reference{kConfigs, options};
    BookBuilder<FastBook> fast{kConfigs, options};
    ReplayComparison comparison{reference, fast, 1}; // full comparison after every order message

    const std::span<const std::uint8_t> rest{data + 1, size - 1};
    const std::vector<std::byte> encoded =
        (data[0] & 1U) != 0 ? encode_operations(rest) : std::vector<std::byte>{};
    const std::span<const std::byte> input =
        (data[0] & 1U) != 0 ? std::span<const std::byte>{encoded} : std::as_bytes(rest);
    const auto parsed = parse(input, comparison);
    if (parsed && *parsed > input.size()) {
        fail("the parser consumed more bytes than it was given");
    }
    if ((data[0] & 1U) != 0 && (!parsed || *parsed != input.size())) {
        fail("the parser rejected bytes the encoder wrote"); // a round-trip check, for free
    }
    comparison.finish();
    if (const auto &first = comparison.first_mismatch(); first.has_value()) {
        std::println(stderr, "fuzz_itch: message {}: {}", first->message, first->what);
        fail("the reference book and the fast book disagree");
    }
    if (reference.stats().structure_failures != 0 || fast.stats().structure_failures != 0) {
        fail("a book's structure check failed");
    }
    return 0;
}

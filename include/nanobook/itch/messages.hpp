/*
 * messages.hpp (itch): the ITCH 5.0 messages NanoBook reads, decoded into plain structs.
 *
 * What it does: One struct per message type the book builder uses (ITCH design §3.3, §3.4):
 *   SystemEvent (S), StockDirectory (R), StockTradingAction (H), AddOrder (A and F),
 *   OrderExecuted (E), OrderExecutedWithPrice (C), OrderCancel (X), OrderDelete (D) and
 *   OrderReplace (U). Also kMessageLength: the spec's fixed length of every message type.
 *
 * Why it exists: The parser turns raw bytes into these structs, so everything after it (the book
 *   builder, the tests) works with named fields instead of byte offsets.
 *
 * How it works: Fields keep ITCH's own units: prices in 1/10,000 of a dollar, shares, Nasdaq's
 *   order reference numbers, nanoseconds since midnight, and text as fixed-size char arrays.
 *   Turning them into NanoBook's types (Price, Qty, OrderId, Side) is the book builder's job
 *   (design §6), so the parser stays a plain decoder that the spec alone can check.
 *
 * Data structures: Small trivially copyable structs, with == for the tests. kMessageLength: 256
 *   entries, indexed by the type letter, 0 for a letter that isn't an ITCH 5.0 type.
 *
 * Trade-offs: Only the fields NanoBook uses are decoded. For example the Stock Directory's market
 *   category and round lot size are skipped; adding one later is one line in the parser.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace nanobook::itch {

using Symbol = std::array<char, 8>; // stock symbol, padded on the right with spaces
using Mpid = std::array<char, 4>;   // market participant id, e.g. "NSDQ"

// Every message starts with the same 11 bytes (design §3.2): type (1), stock locate (2),
// tracking number (2, not decoded), timestamp (6). So every struct has locate and timestamp.

struct SystemEvent { // S
    std::uint16_t locate{};
    std::uint64_t timestamp{}; // nanoseconds since midnight
    char event_code{};         // O, S, Q, M, E, C (design §3.4)

    bool operator==(const SystemEvent &) const = default;
};

struct StockDirectory { // R
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    Symbol stock{};

    bool operator==(const StockDirectory &) const = default;
};

struct StockTradingAction { // H
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    Symbol stock{};
    char trading_state{}; // H halted, P paused, Q quotation only, T trading

    bool operator==(const StockTradingAction &) const = default;
};

struct AddOrder { // A, and F (which adds `attribution`)
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t order_ref{};
    char side{}; // B or S
    std::uint32_t shares{};
    Symbol stock{};
    std::uint32_t price{}; // 1/10,000 of a dollar
    Mpid attribution{};    // F only; all '\0' for A

    bool operator==(const AddOrder &) const = default;
};

struct OrderExecuted { // E
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t order_ref{};
    std::uint32_t executed_shares{};
    std::uint64_t match_number{};

    bool operator==(const OrderExecuted &) const = default;
};

struct OrderExecutedWithPrice { // C
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t order_ref{};
    std::uint32_t executed_shares{};
    std::uint64_t match_number{};
    char printable{};                // Y or N
    std::uint32_t execution_price{}; // 1/10,000 of a dollar

    bool operator==(const OrderExecutedWithPrice &) const = default;
};

struct OrderCancel { // X: part of the order is cancelled; it keeps its place in the queue
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t order_ref{};
    std::uint32_t cancelled_shares{};

    bool operator==(const OrderCancel &) const = default;
};

struct OrderDelete { // D
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t order_ref{};

    bool operator==(const OrderDelete &) const = default;
};

struct OrderReplace { // U: same side and stock as the original order (the spec)
    std::uint16_t locate{};
    std::uint64_t timestamp{};
    std::uint64_t original_order_ref{};
    std::uint64_t new_order_ref{};
    std::uint32_t shares{}; // the new total
    std::uint32_t price{};  // the new price, 1/10,000 of a dollar

    bool operator==(const OrderReplace &) const = default;
};

static_assert(std::is_trivially_copyable_v<SystemEvent> &&
              std::is_trivially_copyable_v<StockDirectory> &&
              std::is_trivially_copyable_v<StockTradingAction> &&
              std::is_trivially_copyable_v<AddOrder> &&
              std::is_trivially_copyable_v<OrderExecuted> &&
              std::is_trivially_copyable_v<OrderExecutedWithPrice> &&
              std::is_trivially_copyable_v<OrderCancel> &&
              std::is_trivially_copyable_v<OrderDelete> &&
              std::is_trivially_copyable_v<OrderReplace>);

// The length of each message type in bytes, from the spec's tables, without the 2-byte length
// prefix. Every type, including the ones the parser skips, so their lengths are checked too.
// All 18 types in the 30 Jan 2019 file had exactly these lengths (design §3.1).
inline constexpr std::array<std::uint8_t, 256> kMessageLength = [] {
    std::array<std::uint8_t, 256> length{};
    constexpr std::array<std::pair<char, std::uint8_t>, 23> kTable{{
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48},
    }};
    for (const auto &[type, len] : kTable) {
        length[static_cast<unsigned char>(type)] = len;
    }
    return length;
}();

} // namespace nanobook::itch

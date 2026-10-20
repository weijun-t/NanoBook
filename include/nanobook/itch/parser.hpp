/*
 * parser.hpp (itch): turns ITCH 5.0 bytes into messages, and hands each one to a handler.
 *
 * What it does: parse() walks a buffer of length-prefixed ITCH messages (design §3.1). For each
 *   whole message it checks the length against the spec, decodes the fields and calls the
 *   handler: handler.on(AddOrder), handler.on(OrderDelete), ..., or handler.on_skipped(type) for
 *   the types NanoBook doesn't use. It stops at an incomplete message at the end of the buffer
 *   and returns how many bytes it consumed. A bad message (unknown type, wrong length) stops it
 *   with a ParseError saying where.
 *
 * Why it exists: The file is raw bytes. Everything after the parser works with named fields.
 *
 * How it works: One pass, O(bytes). Per message: read the 2-byte length, look up the type's
 *   length in kMessageLength (one array read), then a switch on the type letter. Each case reads
 *   its fields at the spec's offsets, written as literal numbers so they can be checked against
 *   the spec's tables line by line. Numbers are big-endian: memcpy into an integer, then
 *   std::byteswap (no reinterpret_cast of the buffer: strict aliasing and alignment UB, README
 *   §4). Stopping at an incomplete message lets a caller feed the file in windows (design §4)
 *   and carry the leftover bytes over.
 *
 * Data structures: None of its own. The handler is a template parameter (a concept, not a base
 *   class): no virtual calls (rule 4), and the compiler can inline the handler's functions.
 *
 * Trade-offs: The first bad message ends the parse. A real feed handler might skip it and go
 *   on, but for a replay that must match Nasdaq exactly, a bad byte means the file is wrong, so
 *   stopping is safer. Errors are std::expected, never exceptions (rule 3).
 */

#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

// ---------------------------------------------------------------------------
// Reading fields (design §5)
// ---------------------------------------------------------------------------

// A big-endian unsigned integer at p. memcpy is the defined way to read bytes as an integer;
// the compiler turns it, plus the byteswap, into one load and one byte-swap instruction.
template <std::unsigned_integral T> [[nodiscard]] inline T read_be(const std::byte *p) {
    T value{};
    std::memcpy(&value, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    } else {
        return value;
    }
}

// The 6-byte timestamp: the top 2 bytes, then the low 4.
[[nodiscard]] inline std::uint64_t read_be48(const std::byte *p) {
    return (std::uint64_t{read_be<std::uint16_t>(p)} << 32U) | read_be<std::uint32_t>(p + 2);
}

[[nodiscard]] inline char read_char(const std::byte *p) {
    return static_cast<char>(*p);
}

template <std::size_t N> [[nodiscard]] inline std::array<char, N> read_text(const std::byte *p) {
    std::array<char, N> text{};
    std::memcpy(text.data(), p, N);
    return text;
}

// ---------------------------------------------------------------------------
// Handler and errors
// ---------------------------------------------------------------------------

// What parse() calls. One on() per decoded message type, and on_skipped() for the rest.
template <typename H>
concept Handler =
    requires(H &h, const SystemEvent &s, const StockDirectory &r, const StockTradingAction &t,
             const AddOrder &a, const OrderExecuted &e, const OrderExecutedWithPrice &c,
             const OrderCancel &x, const OrderDelete &d, const OrderReplace &u, char type) {
        h.on(s);
        h.on(r);
        h.on(t);
        h.on(a); // A and F
        h.on(e);
        h.on(c);
        h.on(x);
        h.on(d);
        h.on(u);
        h.on_skipped(type);
    };

enum class ParseErrorKind : std::uint8_t {
    UnknownType, // the type letter isn't an ITCH 5.0 message type
    WrongLength, // the length prefix doesn't match the spec's length for this type (or is 0)
};

struct ParseError {
    ParseErrorKind kind{};
    std::size_t offset{};   // where the bad message's length prefix starts in the buffer
    char type{};            // '\0' if the length was 0, so there was no type byte
    std::uint16_t length{}; // the length prefix as read

    bool operator==(const ParseError &) const = default;
};

inline constexpr std::size_t kLengthPrefix = 2;

// ---------------------------------------------------------------------------
// Decoding: the offsets are the spec's (design §3.3, §3.4). m points at the type byte.
// ---------------------------------------------------------------------------

template <Handler H> inline void dispatch(const std::byte *m, H &handler) {
    const auto locate = read_be<std::uint16_t>(m + 1);
    const std::uint64_t timestamp = read_be48(m + 5);
    switch (read_char(m)) {
    case 'S':
        handler.on(
            SystemEvent{.locate = locate, .timestamp = timestamp, .event_code = read_char(m + 11)});
        break;
    case 'R':
        handler.on(StockDirectory{
            .locate = locate, .timestamp = timestamp, .stock = read_text<8>(m + 11)});
        break;
    case 'H':
        handler.on(StockTradingAction{.locate = locate,
                                      .timestamp = timestamp,
                                      .stock = read_text<8>(m + 11),
                                      .trading_state = read_char(m + 19)});
        break;
    case 'A':
    case 'F': {
        AddOrder add{.locate = locate,
                     .timestamp = timestamp,
                     .order_ref = read_be<std::uint64_t>(m + 11),
                     .side = read_char(m + 19),
                     .shares = read_be<std::uint32_t>(m + 20),
                     .stock = read_text<8>(m + 24),
                     .price = read_be<std::uint32_t>(m + 32),
                     .attribution = {}};
        if (read_char(m) == 'F') {
            add.attribution = read_text<4>(m + 36);
        }
        handler.on(add);
        break;
    }
    case 'E':
        handler.on(OrderExecuted{.locate = locate,
                                 .timestamp = timestamp,
                                 .order_ref = read_be<std::uint64_t>(m + 11),
                                 .executed_shares = read_be<std::uint32_t>(m + 19),
                                 .match_number = read_be<std::uint64_t>(m + 23)});
        break;
    case 'C':
        handler.on(OrderExecutedWithPrice{.locate = locate,
                                          .timestamp = timestamp,
                                          .order_ref = read_be<std::uint64_t>(m + 11),
                                          .executed_shares = read_be<std::uint32_t>(m + 19),
                                          .match_number = read_be<std::uint64_t>(m + 23),
                                          .printable = read_char(m + 31),
                                          .execution_price = read_be<std::uint32_t>(m + 32)});
        break;
    case 'X':
        handler.on(OrderCancel{.locate = locate,
                               .timestamp = timestamp,
                               .order_ref = read_be<std::uint64_t>(m + 11),
                               .cancelled_shares = read_be<std::uint32_t>(m + 19)});
        break;
    case 'D':
        handler.on(OrderDelete{
            .locate = locate, .timestamp = timestamp, .order_ref = read_be<std::uint64_t>(m + 11)});
        break;
    case 'U':
        handler.on(OrderReplace{.locate = locate,
                                .timestamp = timestamp,
                                .original_order_ref = read_be<std::uint64_t>(m + 11),
                                .new_order_ref = read_be<std::uint64_t>(m + 19),
                                .shares = read_be<std::uint32_t>(m + 27),
                                .price = read_be<std::uint32_t>(m + 31)});
        break;
    default: // a known type NanoBook doesn't decode (parse() has already rejected unknown ones)
        handler.on_skipped(read_char(m));
        break;
    }
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

// Parses every whole message in `bytes`, in order. Returns the number of bytes consumed: all of
// them, or fewer if the buffer ends in the middle of a message (the caller decides whether that
// is an error, e.g. at the end of the file, or the start of the next window).
template <Handler H>
[[nodiscard]] std::expected<std::size_t, ParseError> parse(std::span<const std::byte> bytes,
                                                           H &handler) {
    std::size_t pos = 0;
    while (bytes.size() - pos >= kLengthPrefix) {
        const auto length = read_be<std::uint16_t>(bytes.data() + pos);
        if (bytes.size() - pos - kLengthPrefix < std::size_t{length}) {
            break; // incomplete message at the end
        }
        if (length == 0) {
            return std::unexpected(ParseError{.kind = ParseErrorKind::WrongLength,
                                              .offset = pos,
                                              .type = '\0',
                                              .length = length}); // no type byte
        }
        const std::byte *m = bytes.data() + pos + kLengthPrefix;
        const std::uint8_t expected = kMessageLength[std::to_integer<unsigned char>(*m)];
        if (expected == 0) {
            return std::unexpected(ParseError{.kind = ParseErrorKind::UnknownType,
                                              .offset = pos,
                                              .type = read_char(m),
                                              .length = length});
        }
        if (expected != length) {
            return std::unexpected(ParseError{.kind = ParseErrorKind::WrongLength,
                                              .offset = pos,
                                              .type = read_char(m),
                                              .length = length});
        }
        dispatch(m, handler);
        pos += kLengthPrefix + length;
    }
    return pos;
}

} // namespace nanobook::itch

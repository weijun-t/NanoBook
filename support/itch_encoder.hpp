/*
 * itch_encoder.hpp: test-only writer of ITCH 5.0 bytes, the parser run backwards.
 *
 * What it does: Writer appends length-prefixed ITCH messages to a byte buffer: one function per
 *   decoded message type (add_order() writes A, add_order_mpid() writes F, ...), skipped() for a
 *   type NanoBook doesn't decode (all fields 0), and malformed() for broken input in tests.
 *
 * Why it exists: Parser tests and the golden replay test (ITCH design §8.3) need ITCH bytes with
 *   known contents. Written by this encoder, they are small and readable, and none of Nasdaq's
 *   data is copied into the repo (design §13 Q6).
 *
 * How it works: Each function makes a zeroed message of the spec's length for its type, writes
 *   the type letter and each field at the spec's offset (big-endian: std::byteswap, then
 *   memcpy), and appends the 2-byte length prefix and the message to the buffer.
 *
 * Trade-offs: It uses the same offsets as the parser, so an offset that is wrong in both would
 *   still pass a round trip. The parser tests therefore also decode bytes written out by hand
 *   from the spec's tables, without this encoder.
 */

#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

class Writer {
public:
    void system_event(const SystemEvent &s) {
        Message m = start('S', s.locate, s.timestamp);
        put_char(m, 11, s.event_code);
        finish(m);
    }
    void stock_directory(const StockDirectory &r) {
        Message m = start('R', r.locate, r.timestamp);
        put_text(m, 11, r.stock);
        finish(m);
    }
    void trading_action(const StockTradingAction &t) {
        Message m = start('H', t.locate, t.timestamp);
        put_text(m, 11, t.stock);
        put_char(m, 19, t.trading_state);
        finish(m);
    }
    void add_order(const AddOrder &a) { // A: `attribution` is not written
        Message m = start('A', a.locate, a.timestamp);
        put_add_fields(m, a);
        finish(m);
    }
    void add_order_mpid(const AddOrder &a) { // F
        Message m = start('F', a.locate, a.timestamp);
        put_add_fields(m, a);
        put_text(m, 36, a.attribution);
        finish(m);
    }
    void executed(const OrderExecuted &e) {
        Message m = start('E', e.locate, e.timestamp);
        put_be(m, 11, e.order_ref);
        put_be(m, 19, e.executed_shares);
        put_be(m, 23, e.match_number);
        finish(m);
    }
    void executed_with_price(const OrderExecutedWithPrice &c) {
        Message m = start('C', c.locate, c.timestamp);
        put_be(m, 11, c.order_ref);
        put_be(m, 19, c.executed_shares);
        put_be(m, 23, c.match_number);
        put_char(m, 31, c.printable);
        put_be(m, 32, c.execution_price);
        finish(m);
    }
    void cancel(const OrderCancel &x) {
        Message m = start('X', x.locate, x.timestamp);
        put_be(m, 11, x.order_ref);
        put_be(m, 19, x.cancelled_shares);
        finish(m);
    }
    void delete_order(const OrderDelete &d) {
        Message m = start('D', d.locate, d.timestamp);
        put_be(m, 11, d.order_ref);
        finish(m);
    }
    void replace(const OrderReplace &u) {
        Message m = start('U', u.locate, u.timestamp);
        put_be(m, 11, u.original_order_ref);
        put_be(m, 19, u.new_order_ref);
        put_be(m, 27, u.shares);
        put_be(m, 31, u.price);
        finish(m);
    }
    // Any other ITCH type (e.g. P, I): the right length, every field 0.
    void skipped(char type) {
        finish(start(type, 0, 0));
    }
    // Broken input: this length prefix, then `type` and length - 1 zero bytes.
    void malformed(std::uint16_t length, char type) {
        Message m(length);
        if (length > 0) {
            m[0] = static_cast<std::byte>(type);
        }
        finish(m);
    }

    [[nodiscard]] std::span<const std::byte> bytes() const {
        return bytes_;
    }

private:
    using Message = std::vector<std::byte>; // one message without its length prefix

    static Message start(char type, std::uint16_t locate, std::uint64_t timestamp) {
        Message m(kMessageLength[static_cast<unsigned char>(type)]);
        m[0] = static_cast<std::byte>(type);
        put_be(m, 1, locate);
        put_be48(m, 5, timestamp);
        return m;
    }
    static void put_add_fields(Message &m, const AddOrder &a) {
        put_be(m, 11, a.order_ref);
        put_char(m, 19, a.side);
        put_be(m, 20, a.shares);
        put_text(m, 24, a.stock);
        put_be(m, 32, a.price);
    }

    template <std::unsigned_integral T>
    static void put_be(Message &m, std::size_t offset, T value) {
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        std::memcpy(m.data() + offset, &value, sizeof(T));
    }
    static void put_be48(Message &m, std::size_t offset, std::uint64_t value) {
        put_be(m, offset, static_cast<std::uint16_t>(value >> 32U));
        put_be(m, offset + 2, static_cast<std::uint32_t>(value));
    }
    static void put_char(Message &m, std::size_t offset, char c) {
        m[offset] = static_cast<std::byte>(c);
    }
    template <std::size_t N>
    static void put_text(Message &m, std::size_t offset, const std::array<char, N> &text) {
        std::memcpy(m.data() + offset, text.data(), N);
    }

    void finish(const Message &m) {
        const auto length = static_cast<std::uint16_t>(m.size());
        bytes_.push_back(static_cast<std::byte>(length >> 8U));
        bytes_.push_back(static_cast<std::byte>(length & 0xFFU));
        bytes_.insert(bytes_.end(), m.begin(), m.end());
    }

    std::vector<std::byte> bytes_;
};

} // namespace nanobook::itch

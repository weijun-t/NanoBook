/*
 * records.hpp: the fixed-size records the pipeline's threads pass through its rings.
 *
 * What it does: FeedRecord carries one ITCH message from the feed thread to the engine thread
 *   (ring 1); TopOfBook carries one stock's best bid and ask from the engine thread to the
 *   publisher (ring 2). encode() turns a parsed ITCH message into a FeedRecord; decode() turns a
 *   FeedRecord back into the same message and hands it to a parser Handler (phase 4's book
 *   builder), so the engine runs the phase 4 code unchanged. End marks the end of the stream.
 *
 * Why it exists: Pipeline design §4. A ring holds one type of fixed size, copied as bytes, so the
 *   ITCH message types (different structs, different sizes) are packed into one record.
 *
 * How it works: One flat struct with a `kind`, like phase 1's Event: fields a kind doesn't use
 *   stay 0. Only what the book builder reads crosses the ring: a Stock Directory's 8-character
 *   symbol travels in `ref` (it has no order ref), an F's 4-character attribution in `new_ref`
 *   (an add has no second ref), a System Event's code and a Trading Action's state in `side`.
 *   Fields the builder never reads (an add's stock name, an execution's match number and price)
 *   are left behind. `sent` is the time-stamp counter when the record was ready to send, for the
 *   end-to-end latency (pipeline design §9).
 *
 * Data structures: FeedRecord 48 bytes, TopOfBook 64 bytes (one cache line), both trivially
 *   copyable, checked by static_assert.
 *
 * Trade-offs: The record is larger than most ITCH messages (a Delete is 19 bytes) but fixed, so
 *   a push is one small copy and the ring needs no length prefixes.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "nanobook/itch/messages.hpp"

namespace nanobook::pipeline {

enum class FeedKind : std::uint8_t {
    Add,               // A
    AddMpid,           // F
    Executed,          // E
    ExecutedWithPrice, // C
    Cancel,            // X
    Delete,            // D
    Replace,           // U
    Directory,         // R
    TradingAction,     // H
    SystemEvent,       // S
    End,               // no more records
};

struct FeedRecord {
    std::uint64_t timestamp{}; // ITCH: nanoseconds since midnight
    std::uint64_t ref{};       // order ref (U: the original); R: the 8-character symbol
    std::uint64_t new_ref{};   // U: the new ref; F: the 4-character attribution
    std::uint64_t sent{};      // time-stamp counter when ready to send (design §9)
    std::uint32_t shares{};
    std::uint32_t price{}; // 1/10,000 of a dollar
    std::uint16_t locate{};
    FeedKind kind{FeedKind::End};
    char side{}; // add: B or S; S: the event code; H: the trading state

    bool operator==(const FeedRecord &) const = default;
};
static_assert(sizeof(FeedRecord) == 48 && std::is_trivially_copyable_v<FeedRecord>);

struct TopOfBook {
    std::uint64_t sequence{};  // 1, 2, 3, ...: the nth update the engine published
    std::uint64_t timestamp{}; // ITCH time of the message that caused it
    std::int64_t bid_price{};  // 0 when the side is empty (prices are at least 1 tick)
    std::int64_t bid_qty{};
    std::int64_t ask_price{};
    std::int64_t ask_qty{};
    std::uint64_t sent{}; // copied from the FeedRecord: the end-to-end latency's start
    std::uint16_t locate{};
    bool end{false}; // no more updates

    bool operator==(const TopOfBook &) const = default;
};
static_assert(sizeof(TopOfBook) == 64 && std::is_trivially_copyable_v<TopOfBook>);

// ---------------------------------------------------------------------------
// ITCH message -> FeedRecord
// ---------------------------------------------------------------------------

namespace detail {
template <std::size_t N> std::uint64_t pack(const std::array<char, N> &text) {
    static_assert(N <= 8);
    std::uint64_t value = 0;
    std::memcpy(&value, text.data(), N);
    return value;
}
template <std::size_t N> std::array<char, N> unpack(std::uint64_t value) {
    std::array<char, N> text{};
    std::memcpy(text.data(), &value, N);
    return text;
}
} // namespace detail

inline FeedRecord encode(const itch::AddOrder &m) {
    const bool mpid = m.attribution != itch::Mpid{};
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = m.order_ref,
                      .new_ref = detail::pack(m.attribution),
                      .shares = m.shares,
                      .price = m.price,
                      .locate = m.locate,
                      .kind = mpid ? FeedKind::AddMpid : FeedKind::Add,
                      .side = m.side};
}
inline FeedRecord encode(const itch::OrderExecuted &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = m.order_ref,
                      .shares = m.executed_shares,
                      .locate = m.locate,
                      .kind = FeedKind::Executed};
}
inline FeedRecord encode(const itch::OrderExecutedWithPrice &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = m.order_ref,
                      .shares = m.executed_shares,
                      .locate = m.locate,
                      .kind = FeedKind::ExecutedWithPrice};
}
inline FeedRecord encode(const itch::OrderCancel &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = m.order_ref,
                      .shares = m.cancelled_shares,
                      .locate = m.locate,
                      .kind = FeedKind::Cancel};
}
inline FeedRecord encode(const itch::OrderDelete &m) {
    return FeedRecord{
        .timestamp = m.timestamp, .ref = m.order_ref, .locate = m.locate, .kind = FeedKind::Delete};
}
inline FeedRecord encode(const itch::OrderReplace &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = m.original_order_ref,
                      .new_ref = m.new_order_ref,
                      .shares = m.shares,
                      .price = m.price,
                      .locate = m.locate,
                      .kind = FeedKind::Replace};
}
inline FeedRecord encode(const itch::StockDirectory &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .ref = detail::pack(m.stock),
                      .locate = m.locate,
                      .kind = FeedKind::Directory};
}
inline FeedRecord encode(const itch::StockTradingAction &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .locate = m.locate,
                      .kind = FeedKind::TradingAction,
                      .side = m.trading_state};
}
inline FeedRecord encode(const itch::SystemEvent &m) {
    return FeedRecord{.timestamp = m.timestamp,
                      .locate = m.locate,
                      .kind = FeedKind::SystemEvent,
                      .side = m.event_code};
}

// Is this an order message (one that changes a book and gets a TopOfBook)?
inline bool is_order(FeedKind kind) {
    return kind <= FeedKind::Replace;
}

// ---------------------------------------------------------------------------
// FeedRecord -> the ITCH message, handed to `handler` (a parser Handler)
// ---------------------------------------------------------------------------

template <typename Handler> void decode(const FeedRecord &r, Handler &handler) {
    switch (r.kind) {
    case FeedKind::Add:
    case FeedKind::AddMpid:
        handler.on(itch::AddOrder{.locate = r.locate,
                                  .timestamp = r.timestamp,
                                  .order_ref = r.ref,
                                  .side = r.side,
                                  .shares = r.shares,
                                  .stock = {},
                                  .price = r.price,
                                  .attribution = detail::unpack<4>(r.new_ref)});
        break;
    case FeedKind::Executed:
        handler.on(itch::OrderExecuted{.locate = r.locate,
                                       .timestamp = r.timestamp,
                                       .order_ref = r.ref,
                                       .executed_shares = r.shares,
                                       .match_number = 0});
        break;
    case FeedKind::ExecutedWithPrice:
        handler.on(itch::OrderExecutedWithPrice{.locate = r.locate,
                                                .timestamp = r.timestamp,
                                                .order_ref = r.ref,
                                                .executed_shares = r.shares,
                                                .match_number = 0,
                                                .printable = 0,
                                                .execution_price = 0});
        break;
    case FeedKind::Cancel:
        handler.on(itch::OrderCancel{.locate = r.locate,
                                     .timestamp = r.timestamp,
                                     .order_ref = r.ref,
                                     .cancelled_shares = r.shares});
        break;
    case FeedKind::Delete:
        handler.on(
            itch::OrderDelete{.locate = r.locate, .timestamp = r.timestamp, .order_ref = r.ref});
        break;
    case FeedKind::Replace:
        handler.on(itch::OrderReplace{.locate = r.locate,
                                      .timestamp = r.timestamp,
                                      .original_order_ref = r.ref,
                                      .new_order_ref = r.new_ref,
                                      .shares = r.shares,
                                      .price = r.price});
        break;
    case FeedKind::Directory:
        handler.on(itch::StockDirectory{
            .locate = r.locate, .timestamp = r.timestamp, .stock = detail::unpack<8>(r.ref)});
        break;
    case FeedKind::TradingAction:
        handler.on(itch::StockTradingAction{
            .locate = r.locate, .timestamp = r.timestamp, .stock = {}, .trading_state = r.side});
        break;
    case FeedKind::SystemEvent:
        handler.on(
            itch::SystemEvent{.locate = r.locate, .timestamp = r.timestamp, .event_code = r.side});
        break;
    case FeedKind::End:
        break;
    }
}

} // namespace nanobook::pipeline

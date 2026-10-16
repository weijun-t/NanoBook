#pragma once

#include <cstdint>
#include <type_traits>

#include "nanobook/types.hpp"

namespace nanobook {

// ---------------------------------------------------------------------------
// Inputs (design §3)
// ---------------------------------------------------------------------------

// What a trader sends to the exchange. No id: the exchange assigns one (design §3.1).
// Field order matters for size: owner (4) + symbol (2) + side (1) + type (1) fill exactly
// 8 bytes. With symbol first, padding would make the struct 32 bytes instead of 24.
struct OrderRequest {
    OwnerId owner{};
    SymbolId symbol{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{}; // ignored for Market orders
    Qty qty{};
};

// What the exchange hands a book: the request plus the id it assigned, minus the symbol
// (each book is for one symbol already).
struct NewOrder {
    OrderId id{};
    OwnerId owner{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{}; // ignored for Market orders
    Qty qty{};
};

// Cancel and Modify name the symbol, as in FIX (tag 55), so the exchange routes them without
// reading its id table (design §3). A book ignores `symbol`.
struct Cancel {
    SymbolId symbol{};
    OrderId id{};
};

struct Modify {
    SymbolId symbol{};
    OrderId id{};
    Price new_price{};
    Qty new_total_qty{}; // total INCLUDING already-filled qty, FIX-style (design §7)
};

static_assert(sizeof(OrderRequest) == 24 && std::is_trivially_copyable_v<OrderRequest>);
static_assert(sizeof(NewOrder) == 32 && std::is_trivially_copyable_v<NewOrder>);
static_assert(sizeof(Cancel) == 16 && std::is_trivially_copyable_v<Cancel>);

// ---------------------------------------------------------------------------
// Events (design §4)
// ---------------------------------------------------------------------------

// Names follow FIX ExecType (tag 150).
enum class EventType : std::uint8_t { New, Trade, Canceled, Replaced, Rejected };

enum class CancelReason : std::uint8_t { None, Requested, SelfMatch, Unfilled, FokKill };

enum class RejectReason : std::uint8_t {
    None,
    InvalidQty,
    InvalidPrice,
    UnknownSymbol, // exchange: symbol not listed
    IdsExhausted,  // exchange: every id of the session has been issued (event id is 0)
    BookFull,
    NoLiquidity,
    UnknownOrder,
    TooLateToCancel,
    UseCancel,
    NoChange,
};

// One flat, fixed-size struct for every event kind, tagged by `type` (no std::variant, no
// pointers; design §12). Fields an event kind does not use stay at their zero defaults, so two
// events compare equal exactly when they mean the same thing. Build events with the make_*
// functions below rather than by hand.
//
// Field meaning per type:
//   New       id
//   Trade     id = aggressor, resting_id, price = trade price, qty = traded qty
//   Canceled  id, qty = amount that will never trade, cancel_reason
//   Replaced  id, price = new price, qty = new total qty
//   Rejected  id, reject_reason
struct Event {
    EventType type{EventType::New};
    CancelReason cancel_reason{CancelReason::None};
    RejectReason reject_reason{RejectReason::None};
    OrderId id{};
    OrderId resting_id{};
    Price price{};
    Qty qty{};

    friend constexpr bool operator==(const Event &, const Event &) = default;

    [[nodiscard]] static constexpr Event make_new(OrderId id) {
        return Event{.type = EventType::New, .id = id};
    }
    [[nodiscard]] static constexpr Event make_trade(OrderId aggressor, OrderId resting, Price price,
                                                    Qty qty) {
        return Event{.type = EventType::Trade,
                     .id = aggressor,
                     .resting_id = resting,
                     .price = price,
                     .qty = qty};
    }
    [[nodiscard]] static constexpr Event make_canceled(OrderId id, Qty qty, CancelReason reason) {
        return Event{.type = EventType::Canceled, .cancel_reason = reason, .id = id, .qty = qty};
    }
    [[nodiscard]] static constexpr Event make_replaced(OrderId id, Price new_price,
                                                       Qty new_total_qty) {
        return Event{
            .type = EventType::Replaced, .id = id, .price = new_price, .qty = new_total_qty};
    }
    [[nodiscard]] static constexpr Event make_rejected(OrderId id, RejectReason reason) {
        return Event{.type = EventType::Rejected, .reject_reason = reason, .id = id};
    }
};

// 3 one-byte tags + 5 bytes padding + 4 eight-byte fields.
static_assert(sizeof(Event) == 40 && std::is_trivially_copyable_v<Event>);

} // namespace nanobook

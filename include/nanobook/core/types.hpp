/*
 * types.hpp: the basic vocabulary of the engine: prices, quantities, ids and limits.
 *
 * What it does: Defines Price, Qty, OrderId, OwnerId and SymbolId, the Side and OrderType enums,
 *   and BookConfig: one book's price band, capacity and maximum order size.
 *
 * Why it exists: Every other file talks in these types. Prices are whole numbers of ticks, never
 *   floating point, because floating point can't compare prices exactly (rule 2).
 *
 * How it works: Each value is a "strong type": a struct with one integer inside. Passing a Qty
 *   where a Price is expected is a compile error, yet the machine code is the same as a plain
 *   integer. Each type only gets the operations that make sense: a Qty can be added, an OwnerId
 *   can only be compared with ==. static_asserts pin every size.
 *
 * Data structures: Plain structs and 1-byte enums. BookConfig::is_valid() checks that
 *   max_orders x max_qty fits in 64 bits, which makes signed overflow impossible in every sum the
 *   book computes.
 *
 * Trade-offs: A little more typing (Price{10} instead of 10) in exchange for a whole class of
 *   mix-up bugs becoming compile errors.
 */

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nanobook {

// Strong types (design §2): each is a one-field struct, so passing a Qty where a Price belongs
// is a compile error. They have the same size and generate the same machine code as the bare
// integer. Every field defaults to 0, so a default-constructed value is never garbage.

// Price in ticks (e.g. 1 tick = $0.01). Never floating point (plan rule 2).
struct Price {
    std::int64_t ticks{};

    friend constexpr auto operator<=>(Price, Price) = default;
};

// Signed on purpose: a negative input is detected and rejected instead of wrapping around.
struct Qty {
    std::int64_t value{};

    friend constexpr auto operator<=>(Qty, Qty) = default;
    friend constexpr Qty operator+(Qty a, Qty b) {
        return Qty{a.value + b.value};
    }
    friend constexpr Qty operator-(Qty a, Qty b) {
        return Qty{a.value - b.value};
    }
    constexpr Qty &operator+=(Qty other) {
        value += other.value;
        return *this;
    }
    constexpr Qty &operator-=(Qty other) {
        value -= other.value;
        return *this;
    }
};

// Assigned by the exchange from one counter for all symbols, starting at 1; 0 means "no id"
// (design §2). Ordered, because "was this id ever issued?" needs <.
struct OrderId {
    std::uint64_t value{};

    friend constexpr auto operator<=>(OrderId, OrderId) = default;
};

// Who sent the order. Only ever compared for equality, by self-match prevention (design §6).
struct OwnerId {
    std::uint32_t value{};

    friend constexpr bool operator==(OwnerId, OwnerId) = default;
};

// Which stock. Symbols are numbered 0 ... n-1, so the exchange finds a book with one array index
// (design §2, §3.1). Only ever compared for equality.
struct SymbolId {
    std::uint16_t value{};

    friend constexpr bool operator==(SymbolId, SymbolId) = default;
};

static_assert(sizeof(Price) == 8 && std::is_trivially_copyable_v<Price>);
static_assert(sizeof(Qty) == 8 && std::is_trivially_copyable_v<Qty>);
static_assert(sizeof(OrderId) == 8 && std::is_trivially_copyable_v<OrderId>);
static_assert(sizeof(OwnerId) == 4 && std::is_trivially_copyable_v<OwnerId>);
static_assert(sizeof(SymbolId) == 2 && std::is_trivially_copyable_v<SymbolId>);

// The whole point of strong types: none of them converts to another.
static_assert(!std::is_convertible_v<Qty, Price> && !std::is_convertible_v<Price, Qty>);
static_assert(!std::is_convertible_v<std::int64_t, Price>);

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t { Limit, Market, IOC, FOK };

// Per-book limits, fixed at construction (design §12). Both books take the same config.
struct BookConfig {
    Price min_price{1};          // price band, inclusive
    Price max_price{1};          // price band, inclusive
    std::uint32_t max_orders{0}; // resting orders; uint32_t because pool indices are uint32_t
    Qty max_qty{0};              // largest total qty of one order (a "fat finger" limit)

    // max_orders * max_qty must fit in Qty: every sum the book computes (a level's qty, the
    // look-first count) adds up at most max_orders resting orders of at most max_qty each, so
    // this one check makes signed overflow (undefined behaviour) impossible.
    [[nodiscard]] constexpr bool is_valid() const {
        return min_price.ticks >= 1 && min_price <= max_price && max_orders > 0 &&
               max_qty.value > 0 &&
               max_qty.value <=
                   std::numeric_limits<std::int64_t>::max() / static_cast<std::int64_t>(max_orders);
    }

    [[nodiscard]] constexpr bool in_band(Price p) const {
        return min_price <= p && p <= max_price;
    }

    // Worst case for one input (design §12): New, one event per resting order touched, and one
    // closing Canceled.
    [[nodiscard]] constexpr std::size_t max_events_per_input() const {
        return std::size_t{max_orders} + 2;
    }
};

} // namespace nanobook

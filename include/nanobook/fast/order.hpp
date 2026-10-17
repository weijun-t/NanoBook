/*
 * order.hpp: one order slot in the fast book, and the "no slot" value.
 *
 * What it does: Defines Order, the 48-byte record the fast book keeps for each resting order, and
 *   the sentinel slot numbers kNone ("no slot") and kFreeTag ("this slot is free").
 *
 * Why it exists: The fast book refers to orders by slot number (a uint32_t index into the pool),
 *   never by pointer. An index is half the size of a pointer, so more fit in a cache line, and it
 *   stays valid even if the pool's memory moves.
 *
 * How it works: `prev` and `next` link the order into its price level's queue (the queue is
 *   "intrusive": its links live inside the orders). While a slot is free, `next` links it into
 *   the pool's free list and `prev` holds kFreeTag, which lets Debug builds catch a slot being
 *   released twice. Total size = leaves + filled.
 *
 * Data structures: A plain struct: four 8-byte fields, two 4-byte links, a 4-byte owner and a
 *   1-byte side, padded to 48 bytes (static_assert, rule 9).
 *
 * Trade-offs: At 48 bytes some orders straddle two 64-byte cache lines. Padding to 64 would avoid
 *   that but fit less per line; fast-book design §4.1 measures both.
 */

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "nanobook/core/types.hpp"

namespace nanobook::fast {

// "No slot": the end of a queue or of the free list.
inline constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
// Stored in `prev` while a slot is free. Never a real slot number either.
inline constexpr std::uint32_t kFreeTag = kNone - 1;

struct Order {
    OrderId id{};
    Qty leaves{};  // still waiting to trade
    Qty filled{};  // traded so far; total = leaves + filled (Modify's total-quantity rule)
    Price price{}; // so cancel and modify find the order's level without searching
    std::uint32_t prev{kNone};
    std::uint32_t next{kNone};
    OwnerId owner{}; // self-match prevention
    Side side{Side::Buy};
};

static_assert(sizeof(Order) == 48 && std::is_trivially_copyable_v<Order>);

} // namespace nanobook::fast

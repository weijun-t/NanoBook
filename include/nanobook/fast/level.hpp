/*
 * level.hpp: one price level of the fast book: its price, its total quantity and its queue.
 *
 * What it does: Defines Level, the record both price-level structures (SortedLevels and
 *   TickLevels) store for each price that has orders.
 *
 * Why it exists: The book reads a level's total without walking its queue (level_qty, and design
 *   invariant 3 becomes a real check), and every level structure stores exactly the same thing,
 *   so they are interchangeable.
 *
 * How it works: `total` is the sum of the queued orders' `leaves`, kept up to date by the book on
 *   every add, fill, shrink and removal. The orders themselves live in the OrderPool; the queue
 *   only links them.
 *
 * Data structures: price (8) + total (8) + IntrusiveQueue (12), padded to 32 bytes: two levels per
 *   64-byte cache line (static_assert, rule 9).
 */

#pragma once

#include <type_traits>

#include "nanobook/core/types.hpp"
#include "nanobook/fast/intrusive_queue.hpp"

namespace nanobook::fast {

struct Level {
    Price price{};
    Qty total{}; // sum of `leaves` of the orders in the queue
    IntrusiveQueue queue{};
};

static_assert(sizeof(Level) == 32 && std::is_trivially_copyable_v<Level>);

} // namespace nanobook::fast

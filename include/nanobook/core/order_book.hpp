#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook {

// The contract every order book must meet (design §11): the reference book today, the fast book
// in phase 2. It's checked at compile time. A type that is missing a function, or has the wrong
// signature, fails to compile, and the error names what is missing. No virtual functions and no
// runtime cost (rule 4).
template <typename B>
concept OrderBook =
    std::constructible_from<B, BookConfig> &&
    requires(B &book, const B &cbook, const NewOrder &order, const Cancel &cancel,
             const Modify &modify, EventBuffer &out, Side side, Price price, OrderId id) {
        // Inputs (design §3, §4): each clears `out`, then writes that input's events.
        book.handle(order, out);
        book.handle(cancel, out);
        book.handle(modify, out);

        // Queries for tests and verification only. They may be slow and may allocate (the
        // vectors), so the engine's hot path never calls them.
        { cbook.best_bid() } -> std::same_as<std::optional<Price>>;
        { cbook.best_ask() } -> std::same_as<std::optional<Price>>;
        { cbook.level_qty(side, price) } -> std::same_as<Qty>;
        { cbook.level_orders(side, price) } -> std::same_as<std::vector<OrderId>>;
        { cbook.leaves(id) } -> std::same_as<std::optional<Qty>>;
        { cbook.order_count() } -> std::same_as<std::size_t>;
        { cbook.resting_ids() } -> std::same_as<std::vector<OrderId>>;
        { cbook.invariants_hold() } -> std::same_as<bool>;
    };

} // namespace nanobook

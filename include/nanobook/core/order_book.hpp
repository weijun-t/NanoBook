/*
 * order_book.hpp: the OrderBook concept, a compile-time checklist every order book must meet.
 *
 * What it does: Lists what a book must provide: construction from a BookConfig, handle() for
 *   NewOrder, Cancel and Modify, and test-only queries (best bid and ask, level quantity, ...).
 *   FeedBook adds what copying another exchange's book needs (phase 4): apply() for the four feed
 *   inputs, structure_holds(), and depth() to compare two copies level by level.
 *
 * Why it exists: There will be two books, reference and fast, that must be interchangeable.
 *   Exchange<Book>, the typed tests and the benchmarks are written once, against this concept,
 *   and work for any book that meets it.
 *
 * How it works: A C++20 concept with a requires-expression: the compiler checks that each listed
 *   expression would compile and returns the right type. Nothing happens at runtime. A book that
 *   is missing a function fails to compile, with an error naming the missing line.
 *
 * Trade-offs: The usual alternative, a base class with virtual functions, costs an indirect call
 *   per operation and stops the compiler inlining (rule 4). A concept gives the same "interface"
 *   at zero runtime cost.
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook {

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

// A book that can also copy another exchange's book from its feed (ITCH design §6.1).
// structure_holds() is invariants_hold() without "not crossed": a copy must accept whatever the
// exchange's book looked like, crossed or not.
template <typename B>
concept FeedBook =
    OrderBook<B> && requires(B &book, const B &cbook, const FeedAdd &add, const FeedReduce &reduce,
                             const FeedRemove &remove, const FeedReplace &replace, Side side) {
        { book.apply(add) } -> std::same_as<FeedResult>;
        { book.apply(reduce) } -> std::same_as<FeedResult>;
        { book.apply(remove) } -> std::same_as<FeedResult>;
        { book.apply(replace) } -> std::same_as<FeedResult>;
        { cbook.structure_holds() } -> std::same_as<bool>;
        // Levels best first, so two copies can be compared level by level (ITCH design §8.2).
        { cbook.depth(side, std::size_t{}) } -> std::same_as<std::vector<std::pair<Price, Qty>>>;
    };

} // namespace nanobook

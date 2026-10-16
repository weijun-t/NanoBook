/*
 * exchange.hpp: the exchange: one book per stock, one order-id counter for all of them.
 *
 * What it does: Receives every message. Gives each new order the next id from one counter for the
 *   whole exchange (like Nasdaq), sends it to its stock's book, and turns a book's "order not
 *   found" into the exact reason.
 *
 * Why it exists: Traders can't choose ids, so they can't send a wrong one, and every rejection gets
 *   an exact reason. Each book sees increasing ids with gaps, exactly like a real ITCH feed.
 *
 * How it works: New order: assign the id, route it to books_[symbol], and if the book accepted it,
 *   record "id -> stock" in the id table. Cancel and Modify name their stock, so they go straight
 *   to the book and the table is not read (the hot path). Only when the book doesn't find the
 *   order does the exchange read the table (the cold path): "this book had it" means
 *   TooLateToCancel, anything else means UnknownOrder.
 *
 * Data structures: books_, a vector of books indexed by SymbolId (one array lookup). The id table,
 *   a vector<uint16_t> indexed by order id: 2 bytes per order of the session, sized once.
 *
 * Trade-offs: The id table costs memory for every order of the day (about 200 MB for 100 million
 *   orders), but it is written once per order and only read on errors. Exchange is a template over
 *   the book type, so the fast book plugs in without virtual functions.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook {

// Everything the exchange needs, fixed at construction (design §3.1, §12).
struct ExchangeConfig {
    std::uint64_t max_orders_per_session{0}; // ids 1 ... this can be issued; size of the id table
    std::vector<BookConfig> books;           // books[s] configures the book for SymbolId{s}

    // At most 65,535 symbols: the id table stores a SymbolId per id, and 0xFFFF is reserved
    // to mean "no book accepted this id".
    [[nodiscard]] bool is_valid() const {
        return max_orders_per_session > 0 &&
               max_orders_per_session < std::numeric_limits<std::uint64_t>::max() &&
               !books.empty() && books.size() < std::numeric_limits<std::uint16_t>::max() &&
               std::ranges::all_of(books, [](const BookConfig &b) { return b.is_valid(); });
    }

    // One input only ever reaches one book, so the biggest book sets the buffer size.
    [[nodiscard]] std::size_t max_events_per_input() const {
        std::size_t most = 1; // an exchange-level reject is one event
        for (const BookConfig &b : books) {
            most = std::max(most, b.max_events_per_input());
        }
        return most;
    }
};

// Design §3.1. Single-threaded, like the books. `OrderBook Book` means only types that meet the
// OrderBook concept are allowed.
template <OrderBook Book> class Exchange {
public:
    explicit Exchange(ExchangeConfig config)
        : config_(std::move(config)),
          // Index = order id. Index 0 is never used (id 0 means "no id"). Allocated once.
          book_of_id_(config_.max_orders_per_session + 1, kNoBook) {
        assert(config_.is_valid());
        books_.reserve(config_.books.size());
        for (const BookConfig &b : config_.books) {
            books_.emplace_back(b);
        }
    }

    // Each call clears `out`, then writes the events for this one input. `out` must hold at
    // least max_events_per_input() events.
    void handle(const OrderRequest &request, EventBuffer &out) {
        assert(out.capacity() >= max_events_per_input());
        out.clear();

        if (next_id_ > config_.max_orders_per_session) {
            out.push(Event::make_rejected(OrderId{0}, RejectReason::IdsExhausted)); // no id left
            return;
        }
        // Assigned on receipt, so even a rejected request has an id the trader can refer to.
        const OrderId id{next_id_++};

        if (!listed(request.symbol)) {
            out.push(Event::make_rejected(id, RejectReason::UnknownSymbol));
            return; // the table entry stays kNoBook
        }

        books_[request.symbol.value].handle(NewOrder{.id = id,
                                                     .owner = request.owner,
                                                     .side = request.side,
                                                     .type = request.type,
                                                     .price = request.price,
                                                     .qty = request.qty},
                                            out);

        // A book answers either one Rejected, or New first. Only accepted ids enter the table.
        if (out.view().front().type != EventType::Rejected) {
            book_of_id_[id.value] = request.symbol.value;
        }
    }

    void handle(const Cancel &cancel, EventBuffer &out) {
        route(cancel, out);
    }

    void handle(const Modify &modify, EventBuffer &out) {
        route(modify, out);
    }

    // Queries, for tests and invariant checks.
    [[nodiscard]] const Book &book(SymbolId symbol) const {
        assert(listed(symbol));
        return books_[symbol.value];
    }
    [[nodiscard]] OrderId next_id() const {
        return OrderId{next_id_};
    }
    [[nodiscard]] std::size_t max_events_per_input() const {
        return config_.max_events_per_input();
    }

    // Every book's own invariants (design §9 rules 1-6), plus rule 7: every resting order's id
    // was issued, and the id table points at the book it rests in.
    [[nodiscard]] bool invariants_hold() const {
        for (std::size_t s = 0; s < books_.size(); ++s) {
            if (!books_[s].invariants_hold()) {
                return false;
            }
            for (const OrderId id : books_[s].resting_ids()) {
                if (!issued(id) || book_of_id_[id.value] != s) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    static constexpr std::uint16_t kNoBook = std::numeric_limits<std::uint16_t>::max();

    [[nodiscard]] bool listed(SymbolId symbol) const {
        return symbol.value < books_.size();
    }

    [[nodiscard]] bool issued(OrderId id) const {
        return id.value != 0 && id.value < next_id_;
    }

    // Cancel and Modify: check what only the exchange knows, then let the book do the rest.
    template <typename Input> void route(const Input &input, EventBuffer &out) {
        assert(out.capacity() >= max_events_per_input());
        out.clear();

        if (!listed(input.symbol)) {
            out.push(Event::make_rejected(input.id, RejectReason::UnknownSymbol));
            return;
        }
        if (!issued(input.id)) {
            out.push(Event::make_rejected(input.id, RejectReason::UnknownOrder));
            return;
        }

        // Hot path: straight to the book. The id table is not read.
        books_[input.symbol.value].handle(input, out);

        // Cold path (design §3.1): the book didn't find the order. If this book had accepted it,
        // it lived here and has finished, so the exact reason is TooLateToCancel.
        const Event &first = out.view().front();
        if (first.type == EventType::Rejected &&
            first.reject_reason == RejectReason::UnknownOrder &&
            book_of_id_[input.id.value] == input.symbol.value) {
            out.clear();
            out.push(Event::make_rejected(input.id, RejectReason::TooLateToCancel));
        }
    }

    ExchangeConfig config_;
    std::vector<Book> books_; // index = SymbolId
    std::vector<std::uint16_t>
        book_of_id_;           // index = OrderId; SymbolId that accepted it, or kNoBook
    std::uint64_t next_id_{1}; // the next id to issue; one counter for all symbols
};

} // namespace nanobook

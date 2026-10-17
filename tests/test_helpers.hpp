/*
 * test_helpers.hpp: shared tools for the tests.
 *
 * What it does: Short builders for inputs and expected events, in the same "owner/id qty@price"
 *   order as the design §10 table: buy(1, 2, 100, 10) is owner 1, id 2, 100 @ 10. Harness and
 *   ExchangeHarness own a book (or an exchange) plus a correctly sized EventBuffer; send() runs one
 *   input, checks every invariant, and returns that input's events. PrintTo makes a failing test
 *   print "Trade id=3 resting=1 30@10" instead of raw bytes.
 *
 * Why it exists: So each test reads like its row in the design table, and every test checks the
 *   invariants after every single input without having to ask.
 *
 * How it works: BookTypes lists every book the typed tests run on: ReferenceBook and FastBook. Each
 *   typed test runs once per book. The harnesses are templates over the book type.
 */

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/exchange.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/names.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/reference/reference_book.hpp"

namespace nanobook {

// GoogleTest calls PrintTo (found by argument-dependent lookup) to show a value in a failure
// message, instead of dumping its raw bytes.
inline void PrintTo(Price p, std::ostream *os) {
    *os << "Price{" << p.ticks << "}";
}
inline void PrintTo(Qty q, std::ostream *os) {
    *os << "Qty{" << q.value << "}";
}
inline void PrintTo(OrderId id, std::ostream *os) {
    *os << "OrderId{" << id.value << "}";
}

inline void PrintTo(const Event &e, std::ostream *os) {
    *os << name(e.type) << " id=" << e.id.value;
    switch (e.type) {
    case EventType::New:
        break;
    case EventType::Trade:
        *os << " resting=" << e.resting_id.value << " " << e.qty.value << "@" << e.price.ticks;
        break;
    case EventType::Canceled:
        *os << " qty=" << e.qty.value << " " << name(e.cancel_reason);
        break;
    case EventType::Replaced:
        *os << " " << e.qty.value << "@" << e.price.ticks;
        break;
    case EventType::Rejected:
        *os << " " << name(e.reject_reason);
        break;
    }
}

namespace test {

// ---- inputs --------------------------------------------------------------

inline NewOrder order(OrderType type, Side side, std::uint32_t owner, std::uint64_t id,
                      std::int64_t qty, std::int64_t price) {
    return NewOrder{.id = OrderId{id},
                    .owner = OwnerId{owner},
                    .side = side,
                    .type = type,
                    .price = Price{price},
                    .qty = Qty{qty}};
}
inline NewOrder limit(Side side, std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                      std::int64_t price) {
    return order(OrderType::Limit, side, owner, id, qty, price);
}
inline NewOrder buy(std::uint32_t owner, std::uint64_t id, std::int64_t qty, std::int64_t price) {
    return limit(Side::Buy, owner, id, qty, price);
}
inline NewOrder sell(std::uint32_t owner, std::uint64_t id, std::int64_t qty, std::int64_t price) {
    return limit(Side::Sell, owner, id, qty, price);
}
// Market orders have no price; the field is set to 0 and ignored (design §3).
inline NewOrder market_buy(std::uint32_t owner, std::uint64_t id, std::int64_t qty) {
    return order(OrderType::Market, Side::Buy, owner, id, qty, 0);
}
inline NewOrder market_sell(std::uint32_t owner, std::uint64_t id, std::int64_t qty) {
    return order(OrderType::Market, Side::Sell, owner, id, qty, 0);
}
inline NewOrder ioc_buy(std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                        std::int64_t price) {
    return order(OrderType::IOC, Side::Buy, owner, id, qty, price);
}
inline NewOrder ioc_sell(std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                         std::int64_t price) {
    return order(OrderType::IOC, Side::Sell, owner, id, qty, price);
}
inline NewOrder fok_buy(std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                        std::int64_t price) {
    return order(OrderType::FOK, Side::Buy, owner, id, qty, price);
}
inline NewOrder fok_sell(std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                         std::int64_t price) {
    return order(OrderType::FOK, Side::Sell, owner, id, qty, price);
}
// Book-level Cancel and Modify: a book ignores the symbol, so it is left at 0.
inline Cancel cancel(std::uint64_t id) {
    return Cancel{.id = OrderId{id}};
}
// New TOTAL qty, including what already traded (design §7).
inline Modify modify(std::uint64_t id, std::int64_t new_total_qty, std::int64_t new_price) {
    return Modify{
        .id = OrderId{id}, .new_price = Price{new_price}, .new_total_qty = Qty{new_total_qty}};
}

// ---- expected events -----------------------------------------------------

inline Event accepted(std::uint64_t id) {
    return Event::make_new(OrderId{id});
}
inline Event trade(std::uint64_t aggressor, std::uint64_t resting, std::int64_t qty,
                   std::int64_t price) {
    return Event::make_trade(OrderId{aggressor}, OrderId{resting}, Price{price}, Qty{qty});
}
inline Event canceled(std::uint64_t id, std::int64_t qty, CancelReason reason) {
    return Event::make_canceled(OrderId{id}, Qty{qty}, reason);
}
inline Event replaced(std::uint64_t id, std::int64_t new_total_qty, std::int64_t new_price) {
    return Event::make_replaced(OrderId{id}, Price{new_price}, Qty{new_total_qty});
}
inline Event rejected(std::uint64_t id, RejectReason reason) {
    return Event::make_rejected(OrderId{id}, reason);
}

// ---- exchange inputs (no id on a request: the exchange assigns it) --------

inline OrderRequest request(SymbolId symbol, OrderType type, Side side, std::uint32_t owner,
                            std::int64_t qty, std::int64_t price) {
    return OrderRequest{.owner = OwnerId{owner},
                        .symbol = symbol,
                        .side = side,
                        .type = type,
                        .price = Price{price},
                        .qty = Qty{qty}};
}
inline OrderRequest req_buy(SymbolId symbol, std::uint32_t owner, std::int64_t qty,
                            std::int64_t price) {
    return request(symbol, OrderType::Limit, Side::Buy, owner, qty, price);
}
inline OrderRequest req_sell(SymbolId symbol, std::uint32_t owner, std::int64_t qty,
                             std::int64_t price) {
    return request(symbol, OrderType::Limit, Side::Sell, owner, qty, price);
}
inline Cancel req_cancel(SymbolId symbol, std::uint64_t id) {
    return Cancel{.symbol = symbol, .id = OrderId{id}};
}
inline Modify req_modify(SymbolId symbol, std::uint64_t id, std::int64_t new_total_qty,
                         std::int64_t new_price) {
    return Modify{.symbol = symbol,
                  .id = OrderId{id},
                  .new_price = Price{new_price},
                  .new_total_qty = Qty{new_total_qty}};
}

// ---- driver --------------------------------------------------------------

// Every book type the tests run against (design §11). Each typed test runs once per type.
using BookTypes = ::testing::Types<ReferenceBook, FastBook>;

// Gives each type a readable name in test output: "BookTest/ReferenceBook.Name" instead of
// "BookTest/0.Name".
struct BookTypeNames {
    template <typename T> static std::string GetName(int /*index*/) {
        if constexpr (std::is_same_v<T, ReferenceBook>) {
            return "ReferenceBook";
        } else if constexpr (std::is_same_v<T, FastBook>) {
            return "FastBook";
        } else {
            return "UnnamedBook";
        }
    }
};

// Owns a book plus a correctly sized event buffer. send() runs one input, checks the design §9
// invariants, and returns that input's events as a vector for easy comparison.
template <OrderBook Book> struct Harness {
    explicit Harness(BookConfig config) : book(config), out(config.max_events_per_input()) {
    }

    template <typename Input> std::vector<Event> send(const Input &input) {
        book.handle(input, out);
        EXPECT_TRUE(book.invariants_hold()) << "invariant broken after this input";
        return {out.view().begin(), out.view().end()};
    }

    Book book;
    EventBuffer out;
};

// The same, for a whole exchange of books.
template <OrderBook Book> struct ExchangeHarness {
    explicit ExchangeHarness(ExchangeConfig config)
        : exchange(std::move(config)), out(exchange.max_events_per_input()) {
    }

    template <typename Input> std::vector<Event> send(const Input &input) {
        exchange.handle(input, out);
        EXPECT_TRUE(exchange.invariants_hold()) << "invariant broken after this input";
        return {out.view().begin(), out.view().end()};
    }

    Exchange<Book> exchange;
    EventBuffer out;
};

} // namespace test
} // namespace nanobook

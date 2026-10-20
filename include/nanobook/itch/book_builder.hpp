/*
 * book_builder.hpp (itch): rebuilds every stock's book from parsed ITCH messages.
 *
 * What it does: BookBuilder is a parser Handler that owns one book per stock locate. It turns
 *   each order message into a feed input on that stock's book (ITCH design §6.1): Add (A, F) ->
 *   FeedAdd, Executed (E, C) and Cancel (X) -> FeedReduce, Delete (D) -> FeedRemove, Replace (U)
 *   -> FeedReplace. It also keeps each stock's name (R), trading state (H) and the market's last
 *   System Event (S). ReplayStats counts every message by type, and every book result by message
 *   kind, so every message of the day is accounted for. In check mode it also watches for crossed
 *   books and checks a book's structure every N order messages (design §8.1).
 *
 * Why it exists: It is the "trading firm copying Nasdaq's book" of phase 4 (design §0.4): the
 *   parser reads, the books store, and this connects them.
 *
 * How it works: Construction builds every book from its BookConfig (sizing.hpp), so replaying
 *   never allocates (rule 1). Per order message: check the locate has a book and a name (only a
 *   Stock Directory message creates a stock), convert ITCH's fields
 *   to NanoBook's types (Price(4) is Price ticks of $0.0001, shares -> Qty, ref -> OrderId, B/S ->
 *   Side), call apply(), count the result. A ref of 0 (NanoBook's "no id") or a side that isn't
 *   B or S never reaches a book; it's counted as bad input.
 *
 * Data structures: std::vector<Book> indexed by locate (index 0 unused), and small vectors of the
 *   same size for names, trading states and "crossed right now". ReplayStats is fixed-size
 *   arrays only.
 *
 * Trade-offs: The handler gets the same AddOrder struct for A and F; an F is told apart by its
 *   attribution (an A's is all '\0'). Only the first 16 crossings are kept in detail; all are
 *   counted.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/itch/messages.hpp"

namespace nanobook::itch {

// The order messages, as rows of the results table.
enum class OrderMessage : std::uint8_t {
    Add,               // A
    AddMpid,           // F
    Executed,          // E
    ExecutedWithPrice, // C
    Cancel,            // X
    Delete,            // D
    Replace,           // U
};
inline constexpr std::size_t kOrderMessages = 7;
inline constexpr std::size_t kFeedResults = 7; // FeedResult's values, Ok ... BookFull

// The moment a stock's book became crossed (best bid >= best ask).
struct CrossedMoment {
    std::uint64_t timestamp{}; // nanoseconds since midnight
    std::uint16_t locate{};
    char trading_state{}; // the stock's last H state ('?' if none yet)
    char system_event{};  // the market's last S event (Q = market open, M = closed, ...)

    bool operator==(const CrossedMoment &) const = default;
};

struct ReplayStats {
    std::array<std::uint64_t, 256> messages{}; // every message, by type letter
    std::array<std::array<std::uint64_t, kFeedResults>, kOrderMessages> results{};
    std::uint64_t unknown_locate{}; // order messages for a locate no R message named
    std::uint64_t bad_input{};      // ref 0, or a side that isn't B or S
    // Check mode only:
    std::uint64_t crossed_starts{};        // times a book went from not crossed to crossed
    std::uint64_t crossed_while_trading{}; // ...of which while the stock was trading normally (T)
    std::uint64_t crossed_messages{};      // order messages after which their book was crossed
    std::array<CrossedMoment, 16> first_crossed{};
    std::uint64_t structure_checks{};
    std::uint64_t structure_failures{};

    [[nodiscard]] std::uint64_t result(OrderMessage kind, FeedResult r) const {
        return results[static_cast<std::size_t>(kind)][static_cast<std::size_t>(r)];
    }
};

struct BuilderOptions {
    bool check{false};                // watch for crossed books
    std::uint64_t structure_every{0}; // check a book's structure every N order messages; 0 = never
};

template <FeedBook Book> class BookBuilder {
public:
    // configs[locate] sizes that stock's book; index 0 is unused (locates start at 1).
    BookBuilder(const std::vector<BookConfig> &configs, BuilderOptions options)
        : options_(options), names_(configs.size()), trading_state_(configs.size(), '?'),
          crossed_(configs.size(), false) {
        books_.reserve(configs.size());
        for (const BookConfig &config : configs) {
            books_.emplace_back(config);
        }
    }

    // ---- the parser Handler -------------------------------------------------

    void on(const SystemEvent &s) {
        count('S');
        system_event_ = s.event_code;
    }
    void on(const StockDirectory &r) {
        count('R');
        if (r.locate < names_.size()) {
            names_[r.locate] = r.stock; // a re-sent R is an update (design §3.5)
        }
    }
    void on(const StockTradingAction &h) {
        count('H');
        if (h.locate < trading_state_.size()) {
            trading_state_[h.locate] = h.trading_state;
        }
    }
    void on(const AddOrder &a) {
        const bool mpid = a.attribution != Mpid{};
        count(mpid ? 'F' : 'A');
        if (a.order_ref == 0 || (a.side != 'B' && a.side != 'S')) {
            ++stats_.bad_input;
            return;
        }
        apply(mpid ? OrderMessage::AddMpid : OrderMessage::Add, a.locate, a.timestamp,
              FeedAdd{.id = OrderId{a.order_ref},
                      .side = a.side == 'B' ? Side::Buy : Side::Sell,
                      .price = Price{a.price},
                      .qty = Qty{a.shares}});
    }
    void on(const OrderExecuted &e) {
        count('E');
        reduce(OrderMessage::Executed, e.locate, e.timestamp, e.order_ref, e.executed_shares);
    }
    void on(const OrderExecutedWithPrice &c) {
        count('C');
        reduce(OrderMessage::ExecutedWithPrice, c.locate, c.timestamp, c.order_ref,
               c.executed_shares);
    }
    void on(const OrderCancel &x) {
        count('X');
        reduce(OrderMessage::Cancel, x.locate, x.timestamp, x.order_ref, x.cancelled_shares);
    }
    void on(const OrderDelete &d) {
        count('D');
        if (d.order_ref == 0) {
            ++stats_.bad_input;
            return;
        }
        apply(OrderMessage::Delete, d.locate, d.timestamp, FeedRemove{.id = OrderId{d.order_ref}});
    }
    void on(const OrderReplace &u) {
        count('U');
        if (u.original_order_ref == 0 || u.new_order_ref == 0) {
            ++stats_.bad_input;
            return;
        }
        apply(OrderMessage::Replace, u.locate, u.timestamp,
              FeedReplace{.old_id = OrderId{u.original_order_ref},
                          .new_id = OrderId{u.new_order_ref},
                          .price = Price{u.price},
                          .qty = Qty{u.shares}});
    }
    void on_skipped(char type) {
        count(type);
    }

    // ---- results ------------------------------------------------------------

    [[nodiscard]] const ReplayStats &stats() const {
        return stats_;
    }
    [[nodiscard]] std::size_t locates() const {
        return books_.size();
    }
    [[nodiscard]] const Book &book(std::uint16_t locate) const {
        return books_.at(locate);
    }
    [[nodiscard]] const Symbol &name(std::uint16_t locate) const {
        return names_.at(locate);
    }
    // The locate whose name is `stock`, or 0 if none (a slow search, for tools).
    [[nodiscard]] std::uint16_t find(const Symbol &stock) const {
        for (std::size_t locate = 1; locate < names_.size(); ++locate) {
            if (names_[locate] == stock) {
                return static_cast<std::uint16_t>(locate);
            }
        }
        return 0;
    }

private:
    void count(char type) {
        ++stats_.messages[static_cast<unsigned char>(type)];
    }

    void reduce(OrderMessage kind, std::uint16_t locate, std::uint64_t timestamp, std::uint64_t ref,
                std::uint32_t shares) {
        if (ref == 0) {
            ++stats_.bad_input;
            return;
        }
        apply(kind, locate, timestamp, FeedReduce{.id = OrderId{ref}, .qty = Qty{shares}});
    }

    template <typename Input>
    void apply(OrderMessage kind, std::uint16_t locate, std::uint64_t timestamp,
               const Input &input) {
        if (locate == 0 || locate >= books_.size() || names_[locate] == Symbol{}) {
            ++stats_.unknown_locate; // no R message named this stock
            return;
        }
        Book &book = books_[locate];
        const FeedResult result = book.apply(input);
        ++stats_.results[static_cast<std::size_t>(kind)][static_cast<std::size_t>(result)];
        if (options_.check) {
            check(book, locate, timestamp);
        }
    }

    // Design §8.1: is this stock's book crossed now, and (every N messages) is it well formed?
    void check(const Book &book, std::uint16_t locate, std::uint64_t timestamp) {
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        const bool crossed = bid && ask && !(*bid < *ask);
        if (crossed) {
            ++stats_.crossed_messages;
            if (!crossed_[locate]) {
                if (stats_.crossed_starts < stats_.first_crossed.size()) {
                    stats_.first_crossed[stats_.crossed_starts] =
                        CrossedMoment{.timestamp = timestamp,
                                      .locate = locate,
                                      .trading_state = trading_state_[locate],
                                      .system_event = system_event_};
                }
                ++stats_.crossed_starts;
                if (trading_state_[locate] == 'T') {
                    ++stats_.crossed_while_trading; // design §8.1: only allowed outside T
                }
            }
        }
        crossed_[locate] = crossed;
        if (options_.structure_every != 0 && ++order_messages_ % options_.structure_every == 0) {
            ++stats_.structure_checks;
            if (!book.structure_holds()) {
                ++stats_.structure_failures;
            }
        }
    }

    BuilderOptions options_;
    std::vector<Book> books_;         // by locate
    std::vector<Symbol> names_;       // by locate
    std::vector<char> trading_state_; // by locate: the last H state
    std::vector<bool> crossed_;       // by locate: crossed after its last order message
    char system_event_{'?'};          // the market's last S event code
    std::uint64_t order_messages_{};  // for structure_every
    ReplayStats stats_;
};

} // namespace nanobook::itch

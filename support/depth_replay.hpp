/*
 * depth_replay.hpp: one stock's book, rebuilt from the day's file, for nanobook_depth to show.
 *
 * What it does: DepthReplay reads an ITCH day one message at a time and applies the chosen
 *   stock's order messages to a FastBook (the engine's book) and a ReferenceBook (phase 1's), the
 *   second as a live check (depth design §4.3). It keeps what the books don't: when each resting
 *   order arrived and who it was attributed to, the stock's latest messages (the tape) and its
 *   last trade. It answers what the screen needs: the best N levels of a side (L2), the queue at
 *   one price with the shares ahead of each order (L3), and where a followed order stands.
 *   advance_to() applies every message up to an ITCH time, step() exactly one of the stock's.
 *   ReplayClock turns real time into ITCH time at a speed, with pause and resume.
 *
 * Why it exists: Depth design, option A: a second firm's copy of one book, in its own process,
 *   so the engine and the pipeline never know it exists. Keeping the replay apart from the screen
 *   lets the tests check it without a terminal.
 *
 * How it works:
 *   - MessageCursor walks the buffer: parse() (parser.hpp) on exactly one message at a time, so
 *     the replay can stop between any two. On a file mapping it lets the kernel take back the
 *     pages it has passed, every 64 MiB, as the pipeline does.
 *   - The filter: System Event (S), Stock Directory (R) and trading state (H) go to both book
 *     builders for every stock (they name the stocks); order messages only when their locate is
 *     the chosen stock's. Everything goes through ReplayComparison (phase 4), which compares the
 *     two books after every order message and in full every `full_every` of them. The first
 *     difference stops the replay right after the message that caused it.
 *   - A message's result (Ok, UnknownOrder, ...) is read from the fast builder's statistics: the
 *     one counter that went up. No counter means it never reached a book (bad input).
 *   - Notes: one per resting order, keyed by its ITCH reference, added by A, F and U, removed
 *     when the book no longer holds the order. An execution (E) has no price on the wire; it's
 *     the order's, from its note.
 *   - Reads go through FastBook's read-only queries (depth(), level_orders(), leaves()). They
 *     allocate, which is fine here: another process, ten redraws a second.
 *
 * Data structures: the two book builders (one book per locate, all but one the smallest valid
 *   book), an unordered_map of notes, a deque of tape entries (newest first, capped).
 *
 * Trade-offs: Every message of the day is parsed, all stocks' (about 368 million), to find the
 *   chosen stock's: simple, and fast enough (a few ns each). The comparison holds references to
 *   the builders, so a DepthReplay can't be copied or moved.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "itch_replay_comparison.hpp"
#include "itch_timing.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/itch/book_builder.hpp"
#include "nanobook/itch/day_stats.hpp"
#include "nanobook/itch/messages.hpp"
#include "nanobook/itch/parser.hpp"
#include "nanobook/itch/sizing.hpp"
#include "nanobook/reference/reference_book.hpp"

namespace nanobook::depth {

using itch::Mpid;
using itch::Symbol;

// "aapl" -> "AAPL    ": ITCH's 8 characters, upper case, padded with spaces. Nothing if empty or
// longer than 8.
inline std::optional<Symbol> to_symbol(std::string_view text) {
    Symbol symbol{};
    if (text.empty() || text.size() > symbol.size()) {
        return std::nullopt;
    }
    symbol.fill(' ');
    for (std::size_t i = 0; i < text.size(); ++i) {
        symbol[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    return symbol;
}

inline std::optional<std::uint16_t> find_locate(const std::vector<Symbol> &directory,
                                                const Symbol &symbol) {
    for (std::size_t locate = 1; locate < directory.size(); ++locate) {
        if (directory[locate] == symbol) {
            return static_cast<std::uint16_t>(locate);
        }
    }
    return std::nullopt;
}

// One config per locate: the chosen stock's sized from its stats (sizing.hpp), every other the
// smallest valid book (its order messages never reach it).
inline std::vector<BookConfig> one_stock_configs(const std::vector<Symbol> &directory,
                                                 const std::map<Symbol, itch::StockStats> &stats,
                                                 std::uint16_t locate,
                                                 const itch::SizingRules &rules = {}) {
    std::vector<BookConfig> configs(directory.size(), itch::kUnusedBook);
    const auto found = stats.find(directory.at(locate));
    configs.at(locate) =
        found != stats.end() ? itch::size_book(found->second, rules) : itch::default_book(rules);
    return configs;
}

// Walks a buffer of ITCH messages one message at a time.
class MessageCursor {
public:
    // release_behind: let the kernel take back passed pages. Only for a read-only FILE mapping
    // (itch_timing.hpp's release_pages()); on ordinary memory it would zero them.
    explicit MessageCursor(std::span<const std::byte> bytes, bool release_behind = false)
        : bytes_(bytes), release_behind_(release_behind) {
    }

    // The next whole message's size with its length prefix, or 0 if none is left (the end, or an
    // incomplete message at the end).
    [[nodiscard]] std::size_t next_size() const {
        if (bytes_.size() - pos_ < itch::kLengthPrefix) {
            return 0;
        }
        const auto length = itch::read_be<std::uint16_t>(bytes_.data() + pos_);
        if (bytes_.size() - pos_ - itch::kLengthPrefix < std::size_t{length}) {
            return 0;
        }
        return itch::kLengthPrefix + length;
    }
    [[nodiscard]] bool done() const {
        return next_size() == 0;
    }

    // The next message's ITCH timestamp. A message too short to hold one gives 0, so it goes to
    // the parser at once, which reports it.
    [[nodiscard]] std::uint64_t next_timestamp() const {
        if (next_size() < itch::kLengthPrefix + kTimestampEnd) {
            return 0;
        }
        return itch::read_be48(bytes_.data() + pos_ + itch::kLengthPrefix + kTimestampAt);
    }

    // Parses the next message into `handler`. Precondition: !done().
    template <itch::Handler H> std::expected<void, itch::ParseError> next(H &handler) {
        const std::size_t size = next_size();
        const auto parsed = itch::parse(bytes_.subspan(pos_, size), handler);
        if (!parsed) {
            itch::ParseError e = parsed.error();
            e.offset += pos_;
            return std::unexpected(e);
        }
        pos_ += size;
        if (release_behind_ && pos_ - released_ >= kReleaseEvery) {
            itch::release_pages(bytes_.subspan(released_, pos_ - released_));
            released_ = pos_;
        }
        return {};
    }

    [[nodiscard]] std::size_t position() const {
        return pos_;
    }

private:
    static constexpr std::size_t kTimestampAt = 5;   // type 1, locate 2, tracking 2
    static constexpr std::size_t kTimestampEnd = 11; // ...then 6 bytes of timestamp
    static constexpr std::size_t kReleaseEvery = std::size_t{64} << 20U;

    std::span<const std::byte> bytes_;
    bool release_behind_;
    std::size_t pos_{0};
    std::size_t released_{0};
};

// What DepthReplay remembers about a resting order, from the message that added it.
struct OrderNote {
    std::uint64_t added{}; // ITCH time it joined its queue (a replace starts it again)
    Side side{Side::Buy};
    Price price{};
    Mpid attribution{}; // F's market participant; all '\0' for A and U
};

enum class TapeKind : std::uint8_t { Add, Execute, Cancel, Delete, Replace };

struct TapeEntry {
    std::uint64_t timestamp{};
    TapeKind kind{TapeKind::Add};
    OrderId id{};     // the order (a replace's original)
    OrderId new_id{}; // a replace's new order; 0 otherwise
    Side side{Side::Buy};
    Qty shares{};  // add: size; execute: traded; cancel: cancelled; delete: what was left;
                   // replace: the new size
    Price price{}; // add, replace: the limit; execute: the trade's; cancel, delete: the order's
    std::optional<FeedResult> result; // nothing: it never reached a book (bad input)
    Mpid attribution{};               // an add's market participant, if any
};

struct LevelRow {
    Price price{};
    Qty shares{};
    std::size_t orders{};
};

struct QueueRow {
    OrderId id{};
    Qty shares{}; // remaining
    Qty ahead{};  // shares in front of it at this price: what must go before it can fill
    std::uint64_t since{};
    Mpid attribution{};
};

struct Trade {
    std::uint64_t timestamp{};
    Price price{};
    Qty shares{};
};

enum class FollowState : std::uint8_t { Resting, Filled, Cancelled, NotResting };

struct FollowStatus {
    OrderId id{};
    FollowState state{FollowState::NotResting};
    Side side{Side::Buy};
    Price price{};
    Qty shares{};        // remaining, while resting
    std::size_t place{}; // 1 = front of its queue, while resting
    Qty ahead{};         // while resting
};

struct ReplayOptions {
    std::size_t tape{64};          // entries kept
    std::uint64_t full_every{256}; // full comparison every N of the stock's order messages
    bool release_behind{false};    // see MessageCursor
};

class DepthReplay {
public:
    using Fast = itch::BookBuilder<FastBook>;
    using Reference = itch::BookBuilder<ReferenceBook>;

    // configs: one per locate (one_stock_configs()); locate: the stock to rebuild.
    DepthReplay(std::span<const std::byte> day, const std::vector<BookConfig> &configs,
                std::uint16_t locate, ReplayOptions options = {})
        : DepthReplay(day, configs, configs, locate, options) {
    }
    // The reference book sized apart: the tests make it smaller to plant a difference.
    DepthReplay(std::span<const std::byte> day, const std::vector<BookConfig> &fast_configs,
                const std::vector<BookConfig> &reference_configs, std::uint16_t locate,
                ReplayOptions options)
        : cursor_(day, options.release_behind), locate_(locate), options_(options),
          fast_(fast_configs, itch::BuilderOptions{}),
          reference_(reference_configs, itch::BuilderOptions{}),
          comparison_(fast_, reference_, options.full_every) {
    }
    DepthReplay(const DepthReplay &) = delete;
    DepthReplay &operator=(const DepthReplay &) = delete;
    DepthReplay(DepthReplay &&) = delete;
    DepthReplay &operator=(DepthReplay &&) = delete;
    ~DepthReplay() = default;

    // Applies every message stamped at or before `itch_ns`; stops early at the end, a broken
    // message or a difference between the books. Returns how many messages it parsed.
    std::uint64_t advance_to(std::uint64_t itch_ns) {
        std::uint64_t parsed = 0;
        while (running() && cursor_.next_timestamp() <= itch_ns) {
            next_message();
            ++parsed;
        }
        return parsed;
    }

    // Applies messages up to and including the chosen stock's next order message. False if the
    // replay stopped first.
    bool step() {
        stock_message_seen_ = false;
        while (running() && !stock_message_seen_) {
            next_message();
        }
        return stock_message_seen_;
    }

    // At the end of the day: every stock compared in full, and the two builders' statistics.
    void finish() {
        comparison_.finish();
    }

    // Can the replay go on? Not at the end, after a broken message, or after a difference.
    [[nodiscard]] bool running() const {
        return !error_ && comparison_.mismatches() == 0 && !cursor_.done();
    }
    [[nodiscard]] bool at_end() const {
        return cursor_.done();
    }
    [[nodiscard]] const std::optional<itch::ParseError> &error() const {
        return error_;
    }
    [[nodiscard]] const std::optional<itch::ComparisonMismatch> &mismatch() const {
        return comparison_.first_mismatch();
    }
    [[nodiscard]] std::uint64_t quick_checks() const {
        return comparison_.quick_checks();
    }
    [[nodiscard]] std::uint64_t full_checks() const {
        return comparison_.full_checks();
    }

    [[nodiscard]] std::uint64_t itch_time() const { // the last parsed message's
        return itch_time_;
    }
    [[nodiscard]] std::uint64_t next_time() const { // the next message's; 0 at the end
        return cursor_.done() ? 0 : cursor_.next_timestamp();
    }
    [[nodiscard]] std::uint64_t messages() const { // every stock's
        return messages_;
    }
    [[nodiscard]] std::uint64_t stock_messages() const { // the chosen stock's order messages
        return stock_messages_;
    }
    [[nodiscard]] std::uint16_t locate() const {
        return locate_;
    }
    [[nodiscard]] const FastBook &book() const {
        return fast_.book(locate_);
    }
    [[nodiscard]] const ReferenceBook &reference() const {
        return reference_.book(locate_);
    }
    [[nodiscard]] char trading_state() const { // the stock's last H state; '?' before one
        return trading_state_;
    }

    // L2: the best `n` levels of a side, best first.
    [[nodiscard]] std::vector<LevelRow> levels(Side side, std::size_t n) const {
        std::vector<LevelRow> rows;
        for (const auto &[price, shares] : book().depth(side, n)) {
            rows.push_back(LevelRow{.price = price,
                                    .shares = shares,
                                    .orders = book().level_orders(side, price).size()});
        }
        return rows;
    }

    // L3: the queue at one price, front first.
    [[nodiscard]] std::vector<QueueRow> queue(Side side, Price price) const {
        std::vector<QueueRow> rows;
        Qty ahead{0};
        for (const OrderId id : book().level_orders(side, price)) {
            const Qty shares = book().leaves(id).value_or(Qty{0});
            const OrderNote *note = find_note(id);
            rows.push_back(QueueRow{.id = id,
                                    .shares = shares,
                                    .ahead = ahead,
                                    .since = note != nullptr ? note->added : 0,
                                    .attribution = note != nullptr ? note->attribution : Mpid{}});
            ahead += shares;
        }
        return rows;
    }

    [[nodiscard]] const std::deque<TapeEntry> &tape() const { // newest first
        return tape_;
    }
    [[nodiscard]] const std::optional<Trade> &last_trade() const {
        return last_trade_;
    }
    [[nodiscard]] const OrderNote *find_note(OrderId id) const {
        const auto found = notes_.find(id.value);
        return found != notes_.end() ? &found->second : nullptr;
    }

    // Follow one order: where it stands in its queue, and how it left. A replace follows the
    // new order (it starts again at the back of its new price's queue).
    void follow(OrderId id) {
        followed_ = id;
        follow_end_.reset();
    }
    void unfollow() {
        followed_.reset();
        follow_end_.reset();
    }
    [[nodiscard]] std::optional<FollowStatus> follow_status() const {
        if (!followed_) {
            return std::nullopt;
        }
        FollowStatus status{.id = *followed_};
        if (follow_end_) {
            status.state = *follow_end_;
            return status;
        }
        const OrderNote *note = find_note(*followed_);
        if (note == nullptr || !book().leaves(*followed_)) {
            return status; // NotResting
        }
        status.state = FollowState::Resting;
        status.side = note->side;
        status.price = note->price;
        for (const QueueRow &row : queue(note->side, note->price)) {
            ++status.place;
            if (row.id == *followed_) {
                status.shares = row.shares;
                status.ahead = row.ahead;
                break;
            }
        }
        return status;
    }

private:
    // The parser's handler: everything goes back to the replay.
    struct Router {
        DepthReplay *replay;

        template <typename Message> void on(const Message &m) {
            replay->route(m);
        }
        void on_skipped(char type) {
            replay->comparison_.on_skipped(type);
        }
    };

    template <typename M>
    static constexpr bool kIsOrderMessage =
        std::is_same_v<M, itch::AddOrder> || std::is_same_v<M, itch::OrderExecuted> ||
        std::is_same_v<M, itch::OrderExecutedWithPrice> || std::is_same_v<M, itch::OrderCancel> ||
        std::is_same_v<M, itch::OrderDelete> || std::is_same_v<M, itch::OrderReplace>;

    void next_message() {
        Router router{this};
        if (const auto parsed = cursor_.next(router); !parsed) {
            error_ = parsed.error();
            return;
        }
        ++messages_;
    }

    template <typename Message> void route(const Message &m) {
        itch_time_ = m.timestamp;
        if constexpr (kIsOrderMessage<Message>) {
            if (m.locate != locate_) {
                return;
            }
            ++stock_messages_;
            stock_message_seen_ = true;
            order(m);
        } else {
            if constexpr (std::is_same_v<Message, itch::StockTradingAction>) {
                if (m.locate == locate_) {
                    trading_state_ = m.trading_state;
                }
            }
            comparison_.on(m);
        }
    }

    // Passes an order message to both books; its result from the fast builder's counters.
    template <typename Message>
    std::optional<FeedResult> apply(itch::OrderMessage kind, const Message &m) {
        const auto row = static_cast<std::size_t>(kind);
        const auto before = fast_.stats().results[row];
        comparison_.on(m);
        const auto &after = fast_.stats().results[row];
        for (std::size_t r = 0; r < after.size(); ++r) {
            if (after[r] != before[r]) {
                return static_cast<FeedResult>(r);
            }
        }
        return std::nullopt;
    }

    void order(const itch::AddOrder &a) {
        const bool mpid = a.attribution != Mpid{};
        const auto result = apply(mpid ? itch::OrderMessage::AddMpid : itch::OrderMessage::Add, a);
        const Side side = a.side == 'B' ? Side::Buy : Side::Sell;
        if (result == FeedResult::Ok) {
            notes_[a.order_ref] = OrderNote{.added = a.timestamp,
                                            .side = side,
                                            .price = Price{a.price},
                                            .attribution = a.attribution};
        }
        record(TapeEntry{.timestamp = a.timestamp,
                         .kind = TapeKind::Add,
                         .id = OrderId{a.order_ref},
                         .side = side,
                         .shares = Qty{a.shares},
                         .price = Price{a.price},
                         .result = result,
                         .attribution = a.attribution});
    }

    void order(const itch::OrderExecuted &e) {
        const OrderNote note = note_or_empty(e.order_ref);
        const auto result = apply(itch::OrderMessage::Executed, e);
        executed(e.timestamp, e.order_ref, note, Qty{e.executed_shares}, note.price, result);
    }

    void order(const itch::OrderExecutedWithPrice &c) {
        const OrderNote note = note_or_empty(c.order_ref);
        const auto result = apply(itch::OrderMessage::ExecutedWithPrice, c);
        executed(c.timestamp, c.order_ref, note, Qty{c.executed_shares}, Price{c.execution_price},
                 result);
    }

    void order(const itch::OrderCancel &x) {
        const OrderNote note = note_or_empty(x.order_ref);
        const auto result = apply(itch::OrderMessage::Cancel, x);
        forget_if_gone(x.order_ref, FollowState::Cancelled);
        record(TapeEntry{.timestamp = x.timestamp,
                         .kind = TapeKind::Cancel,
                         .id = OrderId{x.order_ref},
                         .side = note.side,
                         .shares = Qty{x.cancelled_shares},
                         .price = note.price,
                         .result = result});
    }

    void order(const itch::OrderDelete &d) {
        const OrderNote note = note_or_empty(d.order_ref);
        const Qty left = book().leaves(OrderId{d.order_ref}).value_or(Qty{0});
        const auto result = apply(itch::OrderMessage::Delete, d);
        forget_if_gone(d.order_ref, FollowState::Cancelled);
        record(TapeEntry{.timestamp = d.timestamp,
                         .kind = TapeKind::Delete,
                         .id = OrderId{d.order_ref},
                         .side = note.side,
                         .shares = left,
                         .price = note.price,
                         .result = result});
    }

    void order(const itch::OrderReplace &u) {
        const OrderNote note = note_or_empty(u.original_order_ref);
        const auto result = apply(itch::OrderMessage::Replace, u);
        if (result == FeedResult::Ok) {
            notes_.erase(u.original_order_ref);
            notes_[u.new_order_ref] =
                OrderNote{.added = u.timestamp, .side = note.side, .price = Price{u.price}};
            if (followed_ == OrderId{u.original_order_ref}) {
                followed_ = OrderId{u.new_order_ref};
            }
        }
        record(TapeEntry{.timestamp = u.timestamp,
                         .kind = TapeKind::Replace,
                         .id = OrderId{u.original_order_ref},
                         .new_id = OrderId{u.new_order_ref},
                         .side = note.side,
                         .shares = Qty{u.shares},
                         .price = Price{u.price},
                         .result = result});
    }

    void executed(std::uint64_t timestamp, std::uint64_t ref, const OrderNote &note, Qty shares,
                  Price price, std::optional<FeedResult> result) {
        if (result == FeedResult::Ok) {
            last_trade_ = Trade{.timestamp = timestamp, .price = price, .shares = shares};
        }
        forget_if_gone(ref, FollowState::Filled);
        record(TapeEntry{.timestamp = timestamp,
                         .kind = TapeKind::Execute,
                         .id = OrderId{ref},
                         .side = note.side,
                         .shares = shares,
                         .price = price,
                         .result = result});
    }

    [[nodiscard]] OrderNote note_or_empty(std::uint64_t ref) const {
        const OrderNote *note = find_note(OrderId{ref});
        return note != nullptr ? *note : OrderNote{};
    }

    // After an execution, cancel or delete: if the order has left the book, drop its note, and
    // if it was followed, say how it left.
    void forget_if_gone(std::uint64_t ref, FollowState how) {
        if (book().leaves(OrderId{ref})) {
            return;
        }
        if (notes_.erase(ref) != 0 && followed_ == OrderId{ref} && !follow_end_) {
            follow_end_ = how;
        }
    }

    void record(const TapeEntry &entry) {
        tape_.push_front(entry);
        if (tape_.size() > options_.tape) {
            tape_.pop_back();
        }
    }

    MessageCursor cursor_;
    std::uint16_t locate_;
    ReplayOptions options_;
    Fast fast_;
    Reference reference_;
    itch::ReplayComparison<Fast, Reference> comparison_; // after the builders: it holds them
    std::optional<itch::ParseError> error_;
    std::uint64_t itch_time_{0};
    std::uint64_t messages_{0};
    std::uint64_t stock_messages_{0};
    bool stock_message_seen_{false};
    char trading_state_{'?'};
    std::unordered_map<std::uint64_t, OrderNote> notes_;
    std::deque<TapeEntry> tape_;
    std::optional<Trade> last_trade_;
    std::optional<OrderId> followed_;
    std::optional<FollowState> follow_end_;
};

// Real time to ITCH time: itch = anchor + (real - real anchor) x speed. Pause freezes it; resume
// and a new speed re-anchor at the current ITCH time, so nothing jumps. Real times are passed in
// (ns from any fixed start), so the tests drive it with made-up times.
class ReplayClock {
public:
    static constexpr double kMinSpeed = 1.0 / 16;
    static constexpr double kMaxSpeed = 1024;

    ReplayClock(std::uint64_t itch_start, double speed, std::uint64_t real_now)
        : itch_anchor_(itch_start), real_anchor_(real_now), speed_(clamp(speed)) {
    }

    [[nodiscard]] std::uint64_t itch_now(std::uint64_t real_now) const {
        if (paused_ || real_now <= real_anchor_) {
            return itch_anchor_;
        }
        const auto elapsed = static_cast<double>(real_now - real_anchor_);
        return itch_anchor_ + static_cast<std::uint64_t>(elapsed * speed_);
    }

    void pause(std::uint64_t real_now) {
        if (!paused_) {
            itch_anchor_ = itch_now(real_now);
            paused_ = true;
        }
    }

    // `itch_at`: where the replay is now. Steps taken while paused may have moved it past the
    // frozen clock; the clock starts again from the later of the two.
    void resume(std::uint64_t real_now, std::uint64_t itch_at) {
        if (paused_) {
            itch_anchor_ = std::max(itch_anchor_, itch_at);
            real_anchor_ = real_now;
            paused_ = false;
        }
    }

    void set_speed(std::uint64_t real_now, double speed) {
        if (!paused_) {
            itch_anchor_ = itch_now(real_now);
            real_anchor_ = real_now;
        }
        speed_ = clamp(speed);
    }

    [[nodiscard]] bool paused() const {
        return paused_;
    }
    [[nodiscard]] double speed() const {
        return speed_;
    }

private:
    static double clamp(double speed) {
        return std::clamp(speed, kMinSpeed, kMaxSpeed);
    }

    std::uint64_t itch_anchor_;
    std::uint64_t real_anchor_;
    double speed_;
    bool paused_{false};
};

} // namespace nanobook::depth

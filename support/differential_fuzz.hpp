/*
 * differential_fuzz.hpp: feeds two order books the same random inputs and compares everything.
 *
 * What it does: run<A, B>() generates a seeded stream of random orders, cancels and modifies,
 *   gives each input to book A (the trusted reference) and book B (the one being checked), and
 *   stops at the first difference: in the events, in the book state, or in either book's
 *   invariants. It returns a Mismatch with everything needed to replay and debug it.
 *
 * Why it exists: The typed tests only check the ~100 situations someone thought of. The fast book's
 *   worst bugs will be in unusual combinations nobody wrote a test for. The reference book is
 *   trusted, so the right answer never has to be known in advance: whatever it does is right.
 *
 * How it works: Generator produces inputs, steered so the interesting cases are common: prices
 *   clustered around a drifting middle (so orders actually trade), cancels and modifies that
 *   mostly hit orders still resting, few owners (so self-matches happen), every order type, edge
 *   values (max_qty, band edges, invalid inputs), and a full book (a quarter of the seeds use a
 *   16-order book). A fuller book gets more cancels, so it doesn't stay full. Events are compared
 *   after every input; the book state (best prices, the top levels order by order, every resting
 *   id) every `state_every` inputs; both books' invariants every `invariants_every` inputs.
 *
 * Data structures: Generator keeps the last 512 orders it sent in a ring buffer, as cancel and
 *   modify targets. run() keeps the last 20 inputs for the report.
 *
 * Trade-offs: The random numbers come straight from std::mt19937_64 (rng() % n), never from
 *   std::uniform_int_distribution: the standard fixes mt19937_64's output exactly but not how a
 *   distribution uses it, so this is the only way a seed replays identically with any compiler
 *   and standard library. Test and tool code only: it allocates and builds strings freely.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/names.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"

namespace nanobook::fuzz {

// ---------------------------------------------------------------------------
// One input, of any kind
// ---------------------------------------------------------------------------

struct Input {
    enum class Kind : std::uint8_t { New, Cancel, Modify };
    Kind kind{Kind::New};
    NewOrder new_order{};
    Cancel cancel{};
    Modify modify{};
};

template <OrderBook Book> void apply(Book &book, const Input &input, EventBuffer &out) {
    switch (input.kind) {
    case Input::Kind::New:
        book.handle(input.new_order, out);
        return;
    case Input::Kind::Cancel:
        book.handle(input.cancel, out);
        return;
    case Input::Kind::Modify:
        book.handle(input.modify, out);
        return;
    }
}

inline std::string describe(const Input &in) {
    const auto num = [](auto v) { return std::to_string(v); };
    switch (in.kind) {
    case Input::Kind::New: {
        const NewOrder &o = in.new_order;
        return "New id=" + num(o.id.value) + " owner=" + num(o.owner.value) + " " +
               std::string{name(o.side)} + " " + std::string{name(o.type)} + " " +
               num(o.qty.value) + "@" + num(o.price.ticks);
    }
    case Input::Kind::Cancel:
        return "Cancel id=" + num(in.cancel.id.value);
    case Input::Kind::Modify:
        return "Modify id=" + num(in.modify.id.value) + " -> total " +
               num(in.modify.new_total_qty.value) + "@" + num(in.modify.new_price.ticks);
    }
    return "?";
}

inline std::string describe(const Event &e) {
    const auto num = [](auto v) { return std::to_string(v); };
    std::string s = std::string{name(e.type)} + " id=" + num(e.id.value);
    switch (e.type) {
    case EventType::New:
        break;
    case EventType::Trade:
        s += " resting=" + num(e.resting_id.value) + " " + num(e.qty.value) + "@" +
             num(e.price.ticks);
        break;
    case EventType::Canceled:
        s += " qty=" + num(e.qty.value) + " " + std::string{name(e.cancel_reason)};
        break;
    case EventType::Replaced:
        s += " total " + num(e.qty.value) + "@" + num(e.price.ticks);
        break;
    case EventType::Rejected:
        s += " " + std::string{name(e.reject_reason)};
        break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// The generator
// ---------------------------------------------------------------------------

class Generator {
public:
    static constexpr std::int64_t kMaxPrice = 1000; // band is [1, kMaxPrice]
    static constexpr std::int64_t kMaxQty = 1000;
    static constexpr std::uint64_t kOwners = 4;
    static constexpr std::size_t kWindow = 512; // recent orders kept as cancel/modify targets

    // max_orders = 0 lets the seed choose: a quarter of seeds get a 16-order book, so "the book is
    // full" happens often; the rest get room for 2000.
    explicit Generator(std::uint64_t seed, std::uint32_t max_orders = 0) : rng_(seed) {
        const std::uint32_t capacity = max_orders != 0 ? max_orders : (chance(25) ? 16U : 2000U);
        config_ = BookConfig{Price{1}, Price{kMaxPrice}, capacity, Qty{kMaxQty}};
    }

    [[nodiscard]] const BookConfig &config() const {
        return config_;
    }

    // The next input. `reference` is only read, to steer: cancels and modifies prefer orders that
    // are still resting, and a fuller book gets more cancels.
    template <OrderBook Book> Input next(const Book &reference) {
        const auto fill_percent =
            static_cast<std::uint64_t>(reference.order_count() * 100 / config_.max_orders);
        const std::uint64_t cancel_percent = 20 + fill_percent / 2; // 20% (empty) to 70% (full)
        const std::uint64_t roll = below(100);
        if (window_size_ > 0 && roll < cancel_percent) {
            return Input{.kind = Input::Kind::Cancel,
                         .cancel = Cancel{.id = cancel_target(reference)}};
        }
        if (window_size_ > 0 && roll < cancel_percent + 15) {
            return make_modify(reference);
        }
        return make_new();
    }

    // Keeps the remembered totals and prices current, so later modifies are realistic.
    void observe(std::span<const Event> events) {
        for (const Event &e : events) {
            if (e.type != EventType::Replaced) {
                continue;
            }
            for (std::size_t i = 0; i < window_size_; ++i) {
                if (window_[i].id == e.id) {
                    window_[i].total = e.qty;
                    window_[i].price = e.price;
                }
            }
        }
    }

private:
    struct Target {
        OrderId id{};
        Qty total{};
        Price price{};
    };

    // A number in [0, n). Straight from mt19937_64, whose output the standard fixes exactly, so a
    // seed replays the same everywhere. (The % bias is far too small to matter here.)
    std::uint64_t below(std::uint64_t n) {
        assert(n > 0);
        return rng_() % n;
    }
    bool chance(std::uint64_t percent) {
        return below(100) < percent;
    }

    void remember(const Target &t) {
        window_[window_next_] = t;
        window_next_ = (window_next_ + 1) % kWindow;
        window_size_ = std::min(window_size_ + 1, kWindow);
    }

    // Mostly a recent order that is still resting: try a few and keep the first one that rests.
    template <OrderBook Book> Target &pick_target(const Book &reference) {
        Target *t = &window_[below(window_size_)];
        for (int tries = 1; tries < 3 && !reference.leaves(t->id); ++tries) {
            t = &window_[below(window_size_)];
        }
        return *t;
    }

    template <OrderBook Book> OrderId cancel_target(const Book &reference) {
        const std::uint64_t r = below(100);
        if (r < 6) {
            return OrderId{last_id_ + 1 + below(10)}; // never issued
        }
        if (r < 8) {
            return OrderId{below(last_id_ + 1)}; // any id so far, including 0 ("no id")
        }
        return pick_target(reference).id;
    }

    Price pick_price(Side side) {
        const std::uint64_t r = below(100);
        if (r < 1) {
            return Price{chance(50) ? 0 : kMaxPrice + 1}; // outside the band: InvalidPrice
        }
        if (r < 3) {
            return Price{chance(50) ? 1 : kMaxPrice}; // exactly on the band edges
        }
        // Usually a few ticks from the middle, sometimes further. A quarter cross the spread.
        const auto offset = static_cast<std::int64_t>(below(8) + (chance(20) ? below(40) : 0));
        const bool aggressive = chance(25);
        const bool above_mid = (side == Side::Buy) == aggressive;
        const std::int64_t p = above_mid ? mid_ + 1 + offset : mid_ - 1 - offset;
        return Price{std::clamp<std::int64_t>(p, 1, kMaxPrice)};
    }

    Qty pick_qty() {
        const std::uint64_t r = below(100);
        if (r < 1) {
            return Qty{-static_cast<std::int64_t>(below(2))}; // 0 or -1: InvalidQty
        }
        if (r < 2) {
            return Qty{kMaxQty + 1}; // just over the limit: InvalidQty
        }
        if (r < 4) {
            return Qty{kMaxQty}; // exactly the limit
        }
        return Qty{static_cast<std::int64_t>(1 + below(100))};
    }

    Input make_new() {
        last_id_ += 1 + (chance(20) ? below(5) : 0); // gaps, like exchange-wide ids
        const Side side = chance(50) ? Side::Buy : Side::Sell;
        const std::uint64_t t = below(100);
        const OrderType type = t < 70   ? OrderType::Limit
                               : t < 82 ? OrderType::IOC
                               : t < 90 ? OrderType::FOK
                                        : OrderType::Market;
        NewOrder order{.id = OrderId{last_id_},
                       .owner = OwnerId{static_cast<std::uint32_t>(1 + below(kOwners))},
                       .side = side,
                       .type = type,
                       .price = pick_price(side),
                       .qty = pick_qty()};
        if (type == OrderType::Market) {
            order.price = Price{0}; // ignored by the book
        }
        remember(Target{.id = order.id, .total = order.qty, .price = order.price});
        if (chance(3)) { // the middle drifts slowly, so trading moves around the band
            mid_ = std::clamp<std::int64_t>(mid_ + (chance(50) ? 1 : -1), 100, kMaxPrice - 100);
        }
        return Input{.kind = Input::Kind::New, .new_order = order};
    }

    template <OrderBook Book> Input make_modify(const Book &reference) {
        const Target &t = pick_target(reference);
        Price price = t.price;
        if (chance(50)) { // a new price nearby: may lose priority, may cross
            const auto shift = static_cast<std::int64_t>(below(11)) - 5;
            price = Price{std::clamp<std::int64_t>(t.price.ticks + shift, 1, kMaxPrice)};
        }
        if (chance(1)) {
            price = Price{0}; // InvalidPrice
        }
        const std::uint64_t r = below(100);
        const std::int64_t known_total = std::max<std::int64_t>(t.total.value, 1);
        Qty total{};
        if (r < 10) {
            total = t.total; // with an unchanged price: NoChange
        } else if (r < 15) {
            total = Qty{0}; // UseCancel
        } else if (r < 25) {
            // At or below what may already have traded: often TooLateToCancel.
            total = Qty{static_cast<std::int64_t>(below(static_cast<std::uint64_t>(known_total)))};
        } else if (r < 27) {
            total = Qty{-1}; // InvalidQty
        } else if (r < 29) {
            total = Qty{kMaxQty + 1}; // InvalidQty
        } else {
            total = Qty{static_cast<std::int64_t>(1 + below(150))};
        }
        return Input{.kind = Input::Kind::Modify,
                     .modify = Modify{.id = t.id, .new_price = price, .new_total_qty = total}};
    }

    std::mt19937_64 rng_;
    BookConfig config_{};
    std::uint64_t last_id_{0};
    std::int64_t mid_{kMaxPrice / 2};
    std::array<Target, kWindow> window_{};
    std::size_t window_size_{0};
    std::size_t window_next_{0};
};

// ---------------------------------------------------------------------------
// Comparing two books
// ---------------------------------------------------------------------------

struct Options {
    std::uint64_t seed{1};
    std::uint64_t ops{100'000};
    std::uint64_t state_every{64};
    std::uint64_t invariants_every{1024};
    std::uint32_t max_orders{0}; // 0: let the seed choose (see Generator)
};

struct Mismatch {
    std::uint64_t seed{};
    std::uint32_t max_orders{}; // as passed in Options (0: chosen by the seed)
    std::uint64_t step{};       // 1-based: the input that exposed it
    std::string what;           // which check failed
    Input input{};
    std::vector<Event> events_a;
    std::vector<Event> events_b;
    std::vector<Input> recent; // the inputs before it, oldest first
};

// Compares what both books report. The top 11 levels of each side are compared order by order;
// deeper levels are covered by order_count and resting_ids.
template <OrderBook A, OrderBook B>
std::optional<std::string> compare_state(const A &a, const B &b) {
    if (a.best_bid() != b.best_bid()) {
        return "best_bid differs";
    }
    if (a.best_ask() != b.best_ask()) {
        return "best_ask differs";
    }
    if (a.order_count() != b.order_count()) {
        return "order_count differs";
    }
    for (const Side side : {Side::Buy, Side::Sell}) {
        const std::optional<Price> best = side == Side::Buy ? a.best_bid() : a.best_ask();
        if (!best) {
            continue;
        }
        for (std::int64_t k = 0; k <= 10; ++k) {
            const Price p{side == Side::Buy ? best->ticks - k : best->ticks + k};
            const std::string where = std::string{name(side)} + " " + std::to_string(p.ticks);
            if (a.level_qty(side, p) != b.level_qty(side, p)) {
                return "level_qty differs at " + where;
            }
            if (a.level_orders(side, p) != b.level_orders(side, p)) {
                return "level_orders (queue order) differs at " + where;
            }
        }
    }
    if (a.resting_ids() != b.resting_ids()) {
        return "resting_ids differ";
    }
    return std::nullopt;
}

// A: the trusted reference. B: the book being checked. Returns the first mismatch, or nothing.
template <OrderBook A, OrderBook B> std::optional<Mismatch> run(const Options &options) {
    Generator gen{options.seed, options.max_orders};
    A a{gen.config()};
    B b{gen.config()};
    EventBuffer out_a{gen.config().max_events_per_input()};
    EventBuffer out_b{gen.config().max_events_per_input()};

    constexpr std::size_t kRecent = 20;
    std::vector<Input> recent; // last kRecent inputs, as a ring
    recent.reserve(kRecent);
    std::size_t recent_next = 0;

    const auto fail = [&](std::uint64_t step, std::string what, const Input &input) {
        Mismatch m{.seed = options.seed,
                   .max_orders = options.max_orders,
                   .step = step,
                   .what = std::move(what),
                   .input = input,
                   .events_a = {out_a.view().begin(), out_a.view().end()},
                   .events_b = {out_b.view().begin(), out_b.view().end()},
                   .recent = {}};
        for (std::size_t i = 0; i < recent.size(); ++i) { // unroll the ring, oldest first
            m.recent.push_back(recent[(recent_next + i) % recent.size()]);
        }
        return std::optional<Mismatch>{std::move(m)};
    };

    for (std::uint64_t step = 1; step <= options.ops; ++step) {
        const Input input = gen.next(a);
        apply(a, input, out_a);
        apply(b, input, out_b);

        const std::span<const Event> ea = out_a.view();
        const std::span<const Event> eb = out_b.view();
        if (!std::ranges::equal(ea, eb)) {
            return fail(step, "events differ", input);
        }
        gen.observe(ea);

        const bool last = step == options.ops;
        if (step % options.state_every == 0 || last) {
            if (std::optional<std::string> diff = compare_state(a, b)) {
                return fail(step, *diff, input);
            }
        }
        if (step % options.invariants_every == 0 || last) {
            if (!a.invariants_hold()) {
                return fail(step, "book A (reference) broke an invariant", input);
            }
            if (!b.invariants_hold()) {
                return fail(step, "book B broke an invariant", input);
            }
        }

        if (recent.size() < kRecent) {
            recent.push_back(input);
        } else {
            recent[recent_next] = input;
        }
        recent_next = (recent_next + 1) % kRecent;
    }
    return std::nullopt;
}

// A human-readable report, ending with the command that replays it.
inline std::string report(const Mismatch &m) {
    std::string s = "FUZZ MISMATCH: " + m.what + "\n";
    s += "  seed " + std::to_string(m.seed) + ", step " + std::to_string(m.step) + "\n";
    s += "  input: " + describe(m.input) + "\n";
    s += "  book A (reference) events:\n";
    for (const Event &e : m.events_a) {
        s += "    " + describe(e) + "\n";
    }
    s += "  book B events:\n";
    for (const Event &e : m.events_b) {
        s += "    " + describe(e) + "\n";
    }
    s += "  inputs before it (oldest first):\n";
    for (const Input &in : m.recent) {
        s += "    " + describe(in) + "\n";
    }
    s += "  replay: nanobook_fuzz --seed " + std::to_string(m.seed) + " --ops " +
         std::to_string(m.step);
    if (m.max_orders != 0) {
        s += " --max-orders " + std::to_string(m.max_orders);
    }
    s += "\n";
    return s;
}

} // namespace nanobook::fuzz

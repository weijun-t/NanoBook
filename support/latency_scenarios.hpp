/*
 * latency_scenarios.hpp: the situations the latency harness times, one operation at a time.
 *
 * What it does: run_scenario<Book>() builds a book in a known state, then times each operation
 *   of one kind (add, cancel, modify, match, a 10-level sweep, cancel or add in a large book, an
 *   add that creates a level deep in the book) or a steered random stream (mixed), recording one
 *   tagged sample per operation in a Recorder. classify() tags a sample from the events the book
 *   produced (latency-harness design §5.3, §6).
 *
 * Why it exists: The same scenarios run on all three books, so their percentiles compare like with
 *   like. Kept in a header (not in the tool) so tests can check that each scenario really times
 *   the operation it claims to.
 *
 * How it works:
 *   - Most scenarios run in batches: a fresh book is built and set up untimed, then a batch of
 *     operations is timed one by one with time_each(). The first batch is a warm-up and isn't
 *     recorded (caches, branch predictor, the book's memory pages). Batches repeat until `ops`
 *     samples are recorded.
 *   - mixed replays the fuzzer's steered stream (support/differential_fuzz.hpp), generated once
 *     with a reference book steering it, so every book sees exactly the same inputs: 200,000
 *     inputs untimed (an "aged" book, as in phase 2's BM_Aged), then `ops` timed.
 *   - Every input is built before timing starts; only book.handle() is between the stamps. (A book
 *     clears the event buffer itself at the start of every input: the OrderBook contract.)
 *   - Throughput mode (Options::throughput) runs the same operations with one pair of stamps
 *     around each whole batch instead of one per operation, so the CPU can overlap operations
 *     as it normally does; ScenarioResult then holds the total ticks and operations (design §6.3).
 *   - If Options::allocations is set (a function returning the program's allocation count), the
 *     allocations made during timed batches are counted, to check rule 1 (design §10).
 *
 * Data structures: Input arrays (std::vector) built before timing; one EventBuffer per run.
 *
 * Trade-offs: Shuffles use rng() % n directly, never std::shuffle, whose algorithm the standard
 *   leaves open: a seed then gives the same order with any standard library (rule 11). mixed keeps
 *   200,000 + ops inputs in memory (about 90 bytes each: ~110 MB for 1,000,000). Tool and test
 * code: setup allocates freely, outside the timed loop.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "differential_fuzz.hpp"
#include "latency_recorder.hpp"
#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/core/types.hpp"
#include "nanobook/reference/reference_book.hpp"

namespace nanobook::latency {

// ---------------------------------------------------------------------------
// Tagging a sample from its events (design §5.3)
// ---------------------------------------------------------------------------

// What one input turned out to be, judged only by the events it produced:
//   reject  anything Rejected
//   match   anything that traded (a crossing order, a sweep, a modify that crossed)
//   cancel  a successful cancel
//   modify  a successful modify that didn't trade
//   add     a new order that rested without trading
//   other   a new order that neither traded nor rested (an IOC or FOK that found nothing)
inline Tag classify(std::span<const Event> events) {
    if (events.empty()) {
        return Tag::Other;
    }
    bool traded = false;
    for (const Event &e : events) {
        if (e.type == EventType::Rejected) {
            return Tag::Reject;
        }
        traded = traded || e.type == EventType::Trade;
    }
    if (traded) {
        return Tag::Match;
    }
    switch (events.front().type) {
    case EventType::Canceled:
        return Tag::Cancel;
    case EventType::Replaced:
        return Tag::Modify;
    case EventType::New: {
        // Did the new order itself get canceled (IOC/FOK, nothing to trade)? Cancels of other ids
        // are self-match cancels of resting orders; the new order still rested.
        const OrderId id = events.front().id;
        const bool gone = std::ranges::any_of(events.subspan(1), [id](const Event &e) {
            return e.type == EventType::Canceled && e.id == id;
        });
        return gone ? Tag::Other : Tag::Add;
    }
    default:
        return Tag::Other;
    }
}

// ---------------------------------------------------------------------------
// Scenarios (design §6.2)
// ---------------------------------------------------------------------------

enum class Scenario : std::uint8_t {
    Add,
    Cancel,
    Modify,
    Match,
    Sweep10,
    LargeCancel,
    LargeAdd,
    DeepInsert,
    Mixed
};

inline constexpr std::array<Scenario, 9> kAllScenarios = {
    Scenario::Add,      Scenario::Cancel,     Scenario::Modify,
    Scenario::Match,    Scenario::Sweep10,    Scenario::LargeCancel,
    Scenario::LargeAdd, Scenario::DeepInsert, Scenario::Mixed};

constexpr std::string_view scenario_name(Scenario s) {
    switch (s) {
    case Scenario::Add:
        return "add";
    case Scenario::Cancel:
        return "cancel";
    case Scenario::Modify:
        return "modify";
    case Scenario::Match:
        return "match";
    case Scenario::Sweep10:
        return "sweep10";
    case Scenario::LargeCancel:
        return "large_cancel";
    case Scenario::LargeAdd:
        return "large_add";
    case Scenario::DeepInsert:
        return "deep_insert";
    case Scenario::Mixed:
        return "mixed";
    }
    return "?";
}

inline std::optional<Scenario> parse_scenario(std::string_view name) {
    for (const Scenario s : kAllScenarios) {
        if (scenario_name(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

struct Options {
    std::size_t ops{1'000'000}; // samples to record (in throughput mode: operations to time)
    std::uint64_t seed{42};
    std::uint64_t (*allocations)() = nullptr; // if set: count allocations during timed batches
    bool throughput{false}; // time whole batches instead of each operation (design §6.3)
};

struct ScenarioResult {
    std::uint64_t timed_allocations{0}; // allocations while timed operations ran (0 if not counted)
    std::uint64_t throughput_ticks{0};  // throughput mode: total ticks of the timed batches
    std::uint64_t throughput_ops{0};    // throughput mode: operations in them
};

// Fisher–Yates shuffle with rng() % n: the same order for a seed with any standard library.
template <typename T> void shuffle(std::vector<T> &v, std::mt19937_64 &rng) {
    for (std::size_t i = v.size(); i > 1; --i) {
        std::swap(v[i - 1], v[static_cast<std::size_t>(rng() % i)]);
    }
}

namespace detail {

// Every price fits [1, 20000]; narrow enough for TickLevels (at most 65,536 ticks).
inline constexpr BookConfig kSmall{Price{1}, Price{20'000}, 20'000, Qty{1'000'000}};
inline constexpr BookConfig kLarge{Price{1}, Price{20'000}, 120'000, Qty{1'000'000}};

inline constexpr std::size_t kBatch = 10'000;
inline constexpr std::size_t kSweeps = 1'000;   // sweep10: sweeps per batch
inline constexpr std::size_t kSweepLevels = 10; // levels emptied by one sweep
inline constexpr std::size_t kLargeOrders = 100'000;
inline constexpr std::size_t kLargeLevels = 10'000;
inline constexpr std::size_t kDeepLevels = 5'000; // deep_insert: levels at 2, 4, ..., 10,000
inline constexpr std::size_t kMixedWarmup = 200'000;

inline NewOrder limit(Side side, std::uint32_t owner, std::uint64_t id, std::int64_t qty,
                      std::int64_t price) {
    return NewOrder{.id = OrderId{id},
                    .owner = OwnerId{owner},
                    .side = side,
                    .type = OrderType::Limit,
                    .price = Price{price},
                    .qty = Qty{qty}};
}

inline std::uint64_t count_allocations(const Options &o) {
    return o.allocations != nullptr ? o.allocations() : 0;
}

// The batch shape (see the file description). setup(book, out) builds the state, untimed;
// op(book, out, i) performs operation i of the batch. The first batch is an unrecorded warm-up.
template <OrderBook Book, typename Setup, typename Op>
ScenarioResult run_batches(Recorder &recorder, const Options &o, const BookConfig &config,
                           std::size_t batch, Setup setup, Op op) {
    EventBuffer out{config.max_events_per_input()};
    std::unique_ptr<Book> book;
    const auto fresh_book = [&] {
        book = std::make_unique<Book>(config); // the previous book is destroyed here, untimed
        setup(*book, out);
    };

    fresh_book(); // warm-up batch, not recorded
    for (std::size_t i = 0; i < batch; ++i) {
        op(*book, out, i);
    }

    ScenarioResult result;
    std::size_t remaining = o.ops;
    while (remaining > 0) {
        const std::size_t n = std::min(batch, remaining);
        fresh_book();
        const std::uint64_t before = count_allocations(o);
        if (o.throughput) { // one pair of stamps around the whole batch
            const Stamp t0 = start();
            for (std::size_t i = 0; i < n; ++i) {
                op(*book, out, i);
            }
            const Stamp t1 = stop();
            result.throughput_ticks += t1.ticks - t0.ticks;
            result.throughput_ops += n;
        } else {
            time_each(
                recorder, n, [&](std::size_t i) { op(*book, out, i); },
                [&](std::size_t) { return classify(out.view()); });
        }
        result.timed_allocations += count_allocations(o) - before;
        remaining -= n;
    }
    return result;
}

// 0, 1, ..., n-1 in a seeded random order.
inline std::vector<std::size_t> permutation(std::size_t n, std::mt19937_64 &rng) {
    std::vector<std::size_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = i;
    }
    shuffle(v, rng);
    return v;
}

// kBatch resting buys of qty 100, spread evenly over 100 prices (1000, 999, ..., 901).
template <OrderBook Book> void rest_small(Book &book, EventBuffer &out) {
    for (std::size_t i = 0; i < kBatch; ++i) {
        book.handle(limit(Side::Buy, 1, i + 1, 100, 1000 - static_cast<std::int64_t>(i % 100)),
                    out);
    }
}

// 100,000 resting buys of qty 10, 10 on each of prices 1 .. 10,000. Rising prices on the first
// pass, so every new level is the new best and goes at the end of a sorted array.
template <OrderBook Book> void rest_large(Book &book, EventBuffer &out) {
    for (std::size_t i = 0; i < kLargeOrders; ++i) {
        book.handle(limit(Side::Buy, 1, i + 1, 10, 1 + static_cast<std::int64_t>(i % kLargeLevels)),
                    out);
    }
}

template <OrderBook Book> ScenarioResult run_mixed(Recorder &recorder, const Options &o) {
    // Generate the whole stream first, steered by a reference book (as BM_Aged does).
    fuzz::Generator gen{o.seed, kSmall.max_orders};
    const BookConfig config = gen.config();
    std::vector<fuzz::Input> inputs;
    inputs.reserve(kMixedWarmup + o.ops);
    {
        ReferenceBook shadow{config};
        EventBuffer shadow_out{config.max_events_per_input()};
        for (std::size_t i = 0; i < kMixedWarmup + o.ops; ++i) {
            const fuzz::Input in = gen.next(shadow);
            fuzz::apply(shadow, in, shadow_out);
            gen.observe(shadow_out.view());
            inputs.push_back(in);
        }
    }

    Book book{config};
    EventBuffer out{config.max_events_per_input()};
    for (std::size_t i = 0; i < kMixedWarmup; ++i) { // the "aged" part, untimed
        fuzz::apply(book, inputs[i], out);
    }
    ScenarioResult result;
    const std::uint64_t before = count_allocations(o);
    if (o.throughput) { // one pair of stamps around the whole stream
        const Stamp t0 = start();
        for (std::size_t i = 0; i < o.ops; ++i) {
            fuzz::apply(book, inputs[kMixedWarmup + i], out);
        }
        const Stamp t1 = stop();
        result.throughput_ticks = t1.ticks - t0.ticks;
        result.throughput_ops = o.ops;
    } else {
        time_each(
            recorder, o.ops,
            [&](std::size_t i) { fuzz::apply(book, inputs[kMixedWarmup + i], out); },
            [&](std::size_t) { return classify(out.view()); });
    }
    result.timed_allocations = count_allocations(o) - before;
    return result;
}

} // namespace detail

// Runs scenario `s` on a `Book`, recording o.ops samples into `recorder` (its capacity must be at
// least o.ops).
template <OrderBook Book>
ScenarioResult run_scenario(Scenario s, Recorder &recorder, const Options &o) {
    using namespace detail;
    std::mt19937_64 rng{o.seed};

    switch (s) {
    case Scenario::Add: // a buy that rests, on 100 prices of an initially empty book
        return run_batches<Book>(
            recorder, o, kSmall, kBatch, [](Book &, EventBuffer &) {},
            [](Book &book, EventBuffer &out, std::size_t i) {
                book.handle(
                    limit(Side::Buy, 1, i + 1, 10, 1000 - static_cast<std::int64_t>(i % 100)), out);
            });

    case Scenario::Cancel: { // cancel one of 10,000 resting orders, in shuffled order
        const std::vector<std::size_t> order = permutation(kBatch, rng);
        return run_batches<Book>(recorder, o, kSmall, kBatch, rest_small<Book>,
                                 [&order](Book &book, EventBuffer &out, std::size_t i) {
                                     book.handle(Cancel{.id = OrderId{order[i] + 1}}, out);
                                 });
    }

    case Scenario::Modify: { // shuffled; even positions reduce (keep priority), odd increase
        const std::vector<std::size_t> order = permutation(kBatch, rng);
        return run_batches<Book>(
            recorder, o, kSmall, kBatch, rest_small<Book>,
            [&order](Book &book, EventBuffer &out, std::size_t i) {
                const std::size_t k = order[i]; // the order rest_small() added as id k + 1
                book.handle(Modify{.id = OrderId{k + 1},
                                   .new_price = Price{1000 - static_cast<std::int64_t>(k % 100)},
                                   .new_total_qty = Qty{i % 2 == 0 ? 50 : 150}},
                            out);
            });
    }

    case Scenario::Match: // a buy of 1 that trades with the front of 10,000 sells of 1
        return run_batches<Book>(
            recorder, o, kSmall, kBatch,
            [](Book &book, EventBuffer &out) {
                for (std::size_t i = 0; i < kBatch; ++i) {
                    book.handle(limit(Side::Sell, 2, i + 1, 1, 500), out);
                }
            },
            [](Book &book, EventBuffer &out, std::size_t i) {
                book.handle(limit(Side::Buy, 1, kBatch + i + 1, 1, 500), out);
            });

    case Scenario::Sweep10: { // a buy of 10 that empties the 10 cheapest of 10,000 levels
        constexpr std::size_t kLevels = kSweeps * kSweepLevels;
        return run_batches<Book>(
            recorder, o, kSmall, kSweeps,
            [](Book &book, EventBuffer &out) {
                // Highest price first: each new ask is the new best, added at the array's end.
                for (std::size_t i = 0; i < kLevels; ++i) {
                    book.handle(
                        limit(Side::Sell, 2, i + 1, 1, static_cast<std::int64_t>(kLevels - i)),
                        out);
                }
            },
            [](Book &book, EventBuffer &out, std::size_t i) {
                book.handle(limit(Side::Buy, 1, kLevels + i + 1,
                                  static_cast<std::int64_t>(kSweepLevels),
                                  static_cast<std::int64_t>(kLevels)),
                            out);
            });
    }

    case Scenario::LargeCancel: { // cancel 10,000 random orders of 100,000
        const std::vector<std::size_t> order = permutation(kLargeOrders, rng);
        return run_batches<Book>(recorder, o, kLarge, kBatch, rest_large<Book>,
                                 [&order](Book &book, EventBuffer &out, std::size_t i) {
                                     book.handle(Cancel{.id = OrderId{order[i] + 1}}, out);
                                 });
    }

    case Scenario::LargeAdd: { // add at a random one of the large book's 10,000 levels
        std::vector<std::int64_t> prices(kBatch);
        for (std::int64_t &p : prices) {
            p = 1 + static_cast<std::int64_t>(rng() % kLargeLevels);
        }
        return run_batches<Book>(
            recorder, o, kLarge, kBatch, rest_large<Book>,
            [&prices](Book &book, EventBuffer &out, std::size_t i) {
                book.handle(limit(Side::Buy, 1, kLargeOrders + i + 1, 10, prices[i]), out);
            });
    }

    case Scenario::DeepInsert: { // a new level at a random depth among 5,000: the sorted
                                 // array's worst case (phase 2 report §5.5)
        // Bids at 2, 4, ..., 10,000; each timed add uses a different odd price, so it always
        // creates a new level, at a random depth. One batch uses each odd price once.
        const std::vector<std::size_t> order = permutation(kDeepLevels, rng);
        return run_batches<Book>(
            recorder, o, kSmall, kDeepLevels,
            [](Book &book, EventBuffer &out) {
                for (std::size_t i = 0; i < kDeepLevels; ++i) {
                    book.handle(
                        limit(Side::Buy, 1, i + 1, 10, 2 * static_cast<std::int64_t>(i + 1)), out);
                }
            },
            [&order](Book &book, EventBuffer &out, std::size_t i) {
                book.handle(limit(Side::Buy, 1, kDeepLevels + i + 1, 10,
                                  2 * static_cast<std::int64_t>(order[i]) + 1),
                            out);
            });
    }

    case Scenario::Mixed:
        return run_mixed<Book>(recorder, o);
    }
    return ScenarioResult{};
}

} // namespace nanobook::latency

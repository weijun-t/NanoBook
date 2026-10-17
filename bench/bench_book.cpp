/*
 * bench_book.cpp: microbenchmarks for order books (Google Benchmark).
 *
 * What it does: Times add, cancel, match, sweeps through 1, 10 and 100 levels, and a seeded random
 *   mix, plus two realistic situations: an "aged" book after a long random trading stream, and a
 *   large book (100,000 orders on 10,000 levels). Counts heap allocations per operation. Results
 * are recorded in docs/perf-log.md.
 *
 * Why it exists: Phase 1's numbers are the "before" the fast book must beat, and rule 10 says no
 *   optimisation without before/after numbers. The aged and large books exist because a fresh,
 *   small book stays in the CPU cache and flatters every structure, std::map most of all
 *   (fast-book design §9).
 *
 * How it works: Every benchmark is a template over the book type, so each book runs exactly the
 *   same code. Most benchmarks build a fresh book with the timer paused, then time a batch of
 *   10,000 operations, so the cost of pausing the timer (over 100 ns) is negligible. The aged book
 *   replays one pre-generated stream (warm-up untimed, then timed batches), so only the book's
 *   work is timed. Random streams use fixed seeds, so every run sees the same input.
 *
 * Trade-offs: Google Benchmark reports averages, not tail latency (p99 comes from the rdtsc harness
 *   in phase 3). Numbers from a laptop, WSL2 or CI only compare books on the same machine: never
 *   publish them (rules 17, 19, 20).
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "alloc_counter.hpp"
#include "differential_fuzz.hpp"
#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/fast/fast_book.hpp"
#include "nanobook/reference/reference_book.hpp"

namespace {

using namespace nanobook;

constexpr std::uint64_t kSeed = 42; // fixed seed: the same input stream on every run (rule 11)
constexpr std::int64_t kBatch = 10'000;

// Room for the small benchmarks. max_orders also sizes the event buffer (40 bytes per event).
// The band is [1, 20000]: every benchmark price fits, and it is narrow enough for the TickLevels
// experiment (at most 65,536 ticks). It was [1, 1000000] until phase 2 step 6; the band doesn't
// change the reference book or the sorted levels, which only store prices that have orders.
constexpr BookConfig kConfig{Price{1}, Price{20'000}, 20'000, Qty{1'000'000}};

NewOrder order(OrderType type, Side side, std::uint32_t owner, std::uint64_t id, std::int64_t qty,
               std::int64_t price) {
    return NewOrder{.id = OrderId{id},
                    .owner = OwnerId{owner},
                    .side = side,
                    .type = type,
                    .price = Price{price},
                    .qty = Qty{qty}};
}

// What one benchmark did, for report(). A struct with named fields rather than two numbers in a
// row, so a call can't swap them (clang-tidy bugprone-easily-swappable-parameters).
struct Totals {
    std::int64_t ops_per_batch{};
    std::uint64_t allocations{}; // over all iterations
};

// Time per operation and allocations per operation.
void report(benchmark::State &state, const Totals &totals) {
    const std::int64_t ops_per_batch = totals.ops_per_batch;
    const double total_ops =
        static_cast<double>(ops_per_batch) * static_cast<double>(state.iterations());
    state.SetItemsProcessed(static_cast<std::int64_t>(total_ops));
    // Rate inverted = seconds per operation; printed with a unit prefix (e.g. 250n = 250 ns).
    state.counters["time_per_op"] = benchmark::Counter(
        static_cast<double>(ops_per_batch),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["allocs_per_op"] = static_cast<double>(totals.allocations) / total_ops;
}

// The shape of most benchmarks: each iteration builds a fresh book from `config`, runs `setup` on
// it with the timer paused, then times `work`, which performs `ops_per_batch` operations. Timing a
// batch instead of one operation keeps the cost of PauseTiming/ResumeTiming (well over 100 ns)
// small next to the work.
template <OrderBook Book, typename Setup, typename Work>
void run_batches(benchmark::State &state, const BookConfig &config, std::int64_t ops_per_batch,
                 Setup setup, Work work) {
    EventBuffer out{config.max_events_per_input()}; // allocated once, reused
    // On the heap, so it can be replaced while paused. (std::optional<Book> works too, but GCC
    // -O3 gives a false -Wmaybe-uninitialized warning inside std::map for it.)
    std::unique_ptr<Book> book;
    std::uint64_t allocations = 0;

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<Book>(config); // the previous book is destroyed here, untimed
        setup(*book, out);
        state.ResumeTiming();

        allocations += support::allocations_during([&] { work(*book, out); });
        benchmark::DoNotOptimize(out.size()); // keep the compiler from discarding the work
    }
    report(state, {.ops_per_batch = ops_per_batch, .allocations = allocations});
}

// ---------------------------------------------------------------------------
// Small, fresh books
// ---------------------------------------------------------------------------

// Adds kBatch buy orders spread evenly over 100 price levels. Nothing to match, so every order
// rests. (Evenly spread: deep levels are hit as often as the best one.)
template <OrderBook Book> void BM_AddResting(benchmark::State &state) {
    run_batches<Book>(
        state, kConfig, kBatch, [](Book &, EventBuffer &) {},
        [](Book &book, EventBuffer &out) {
            for (std::int64_t i = 0; i < kBatch; ++i) {
                book.handle(order(OrderType::Limit, Side::Buy, 1, static_cast<std::uint64_t>(i + 1),
                                  10, 1000 - i % 100),
                            out);
            }
        });
}

// Cancels kBatch resting orders in a shuffled (but fixed) order, like real cancels that hit
// orders all over the book rather than front to back.
template <OrderBook Book> void BM_Cancel(benchmark::State &state) {
    std::vector<std::uint64_t> ids(kBatch);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ids[i] = i + 1;
    }
    std::mt19937_64 rng{kSeed};
    std::ranges::shuffle(ids, rng);

    run_batches<Book>(
        state, kConfig, kBatch,
        [](Book &book, EventBuffer &out) {
            for (std::int64_t i = 0; i < kBatch; ++i) {
                book.handle(order(OrderType::Limit, Side::Buy, 1, static_cast<std::uint64_t>(i + 1),
                                  10, 1000 - i % 100),
                            out);
            }
        },
        [&ids](Book &book, EventBuffer &out) {
            for (const std::uint64_t id : ids) {
                book.handle(Cancel{.id = OrderId{id}}, out);
            }
        });
}

// kBatch sells of qty 1 rest at one price; then kBatch buys of qty 1 each trade with the front
// one. Every timed operation is exactly one New + one Trade.
template <OrderBook Book> void BM_MatchOne(benchmark::State &state) {
    run_batches<Book>(
        state, kConfig, kBatch,
        [](Book &book, EventBuffer &out) {
            for (std::int64_t i = 0; i < kBatch; ++i) {
                book.handle(order(OrderType::Limit, Side::Sell, 2,
                                  static_cast<std::uint64_t>(i + 1), 1, 500),
                            out);
            }
        },
        [](Book &book, EventBuffer &out) {
            for (std::int64_t i = 0; i < kBatch; ++i) {
                book.handle(order(OrderType::Limit, Side::Buy, 1,
                                  static_cast<std::uint64_t>(kBatch + i + 1), 1, 500),
                            out);
            }
        });
}

// One buy order sweeps through N price levels (one order of qty 1 on each). state.range(0) = N.
// 100 sweeps per batch; time_per_op is per sweep, so it shows how the cost grows with N.
template <OrderBook Book> void BM_Sweep(benchmark::State &state) {
    constexpr std::int64_t kSweeps = 100;
    const std::int64_t levels = state.range(0);
    const std::int64_t total = kSweeps * levels;

    run_batches<Book>(
        state, kConfig, kSweeps,
        [total](Book &book, EventBuffer &out) {
            // Highest price first, so each new ask is the new best. In a sorted array the best
            // level is the last one, so every level is added at the end and nothing has to move.
            // (Lowest-first made every add shift every level: 50 million moves to build one
            // book. The book built is the same either way.)
            for (std::int64_t i = 0; i < total; ++i) {
                book.handle(order(OrderType::Limit, Side::Sell, 2,
                                  static_cast<std::uint64_t>(i + 1), 1, total - i),
                            out);
            }
        },
        [levels, total](Book &book, EventBuffer &out) {
            for (std::int64_t s = 0; s < kSweeps; ++s) {
                const auto id = static_cast<std::uint64_t>(total + s + 1);
                // Takes the N cheapest levels that are left.
                book.handle(order(OrderType::Limit, Side::Buy, 1, id, levels, total), out);
            }
        });
}

// A seeded random stream: 60% passive adds near the middle, 30% cancels of earlier orders (some
// have already traded, so those cancels are rejected, as in real life), 10% aggressive IOCs.
// Generated once, before timing, so only the book's work is measured.
struct Op {
    bool is_cancel{false};
    NewOrder new_order{};
    Cancel cancel{};
};

std::vector<Op> mixed_stream(std::int64_t count) {
    std::mt19937_64 rng{kSeed};
    std::uniform_int_distribution<int> percent{0, 99};
    std::uniform_int_distribution<std::int64_t> qty{1, 100};
    std::uniform_int_distribution<std::int64_t> offset{0, 19};
    std::uniform_int_distribution<std::uint32_t> owner{1, 10};

    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(count));
    std::uint64_t next_id = 1;
    for (std::int64_t i = 0; i < count; ++i) {
        const int roll = percent(rng);
        if (roll < 30 && next_id > 1) {
            std::uniform_int_distribution<std::uint64_t> pick{1, next_id - 1};
            ops.push_back(Op{.is_cancel = true, .cancel = Cancel{.id = OrderId{pick(rng)}}});
        } else if (roll < 40) {
            const bool buy = percent(rng) < 50;
            ops.push_back(Op{.new_order = order(OrderType::IOC, buy ? Side::Buy : Side::Sell,
                                                owner(rng), next_id++, qty(rng), buy ? 520 : 480)});
        } else {
            const bool buy = percent(rng) < 50;
            const std::int64_t price = buy ? 499 - offset(rng) : 501 + offset(rng);
            ops.push_back(Op{.new_order = order(OrderType::Limit, buy ? Side::Buy : Side::Sell,
                                                owner(rng), next_id++, qty(rng), price)});
        }
    }
    return ops;
}

template <OrderBook Book> void BM_Mixed(benchmark::State &state) {
    const std::vector<Op> ops = mixed_stream(kBatch);
    run_batches<Book>(
        state, kConfig, kBatch, [](Book &, EventBuffer &) {},
        [&ops](Book &book, EventBuffer &out) {
            for (const Op &op : ops) {
                if (op.is_cancel) {
                    book.handle(op.cancel, out);
                } else {
                    book.handle(op.new_order, out);
                }
            }
        });
}

// ---------------------------------------------------------------------------
// Large book: 100,000 resting orders on 10,000 levels, too big for the CPU's fast caches
// ---------------------------------------------------------------------------

constexpr std::int64_t kLargeOrders = 100'000;
constexpr std::int64_t kLargeLevels = 10'000;
constexpr std::int64_t kTimedBatches = 50;   // fixed, so a slow book can't make the run balloon
constexpr std::int64_t kSweepBatches = 1000; // 100,000 sweeps per measurement
constexpr BookConfig kLargeConfig{Price{1}, Price{20'000}, 120'000, Qty{1'000'000}};

// 10 buy orders on each of prices 1 ... 10,000. The first pass adds levels in rising price order,
// so each new bid is the new best and goes at the end of a sorted array.
template <OrderBook Book> void fill_large(Book &book, EventBuffer &out) {
    for (std::int64_t i = 0; i < kLargeOrders; ++i) {
        book.handle(order(OrderType::Limit, Side::Buy, 1, static_cast<std::uint64_t>(i + 1), 10,
                          1 + i % kLargeLevels),
                    out);
    }
}

// Cancels kBatch randomly chosen orders out of the 100,000.
template <OrderBook Book> void BM_LargeCancel(benchmark::State &state) {
    std::vector<std::uint64_t> ids(static_cast<std::size_t>(kLargeOrders));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ids[i] = i + 1;
    }
    std::mt19937_64 rng{kSeed};
    std::ranges::shuffle(ids, rng);
    ids.resize(kBatch);

    run_batches<Book>(state, kLargeConfig, kBatch, fill_large<Book>,
                      [&ids](Book &book, EventBuffer &out) {
                          for (const std::uint64_t id : ids) {
                              book.handle(Cancel{.id = OrderId{id}}, out);
                          }
                      });
}

// Adds kBatch more buy orders at random prices among the 10,000 existing levels: every add has to
// find its level in a big book.
template <OrderBook Book> void BM_LargeAdd(benchmark::State &state) {
    std::vector<std::int64_t> prices(kBatch);
    std::mt19937_64 rng{kSeed};
    for (std::int64_t &p : prices) {
        p = 1 + static_cast<std::int64_t>(rng() % kLargeLevels);
    }
    run_batches<Book>(state, kLargeConfig, kBatch, fill_large<Book>,
                      [&prices](Book &book, EventBuffer &out) {
                          std::uint64_t id = kLargeOrders + 1;
                          for (const std::int64_t p : prices) {
                              book.handle(order(OrderType::Limit, Side::Buy, 1, id++, 10, p), out);
                          }
                      });
}

// ---------------------------------------------------------------------------
// Aged book: after a long random "trading day", memory is no longer fresh and tidy
// ---------------------------------------------------------------------------

constexpr std::uint32_t kAgedMaxOrders = 20'000;
constexpr std::size_t kAgedWarmup = 200'000;

// The fuzzer's steered random stream (orders, cancels, modifies of every kind), generated once and
// shared by every book. It is steered by a reference book; every book reacts identically (the
// fuzzer proves it), so replaying it puts each book in the same state.
const std::vector<fuzz::Input> &aged_stream() {
    static const std::vector<fuzz::Input> stream = [] {
        fuzz::Generator gen{kSeed, kAgedMaxOrders};
        ReferenceBook shadow{gen.config()};
        EventBuffer out{gen.config().max_events_per_input()};
        const std::size_t count = kAgedWarmup + static_cast<std::size_t>(kTimedBatches * kBatch);
        std::vector<fuzz::Input> inputs;
        inputs.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const fuzz::Input in = gen.next(shadow);
            fuzz::apply(shadow, in, out);
            gen.observe(out.view());
            inputs.push_back(in);
        }
        return inputs;
    }();
    return stream;
}

// Untimed: build the book and replay the 200,000-input warm-up. Timed: the next kBatch inputs per
// iteration, on the same, ageing book.
template <OrderBook Book> void BM_Aged(benchmark::State &state) {
    const std::vector<fuzz::Input> &inputs = aged_stream();
    const BookConfig config = fuzz::Generator{kSeed, kAgedMaxOrders}.config();
    Book book{config};
    EventBuffer out{config.max_events_per_input()};
    for (std::size_t i = 0; i < kAgedWarmup; ++i) {
        fuzz::apply(book, inputs[i], out);
    }

    std::size_t next = kAgedWarmup;
    std::uint64_t allocations = 0;
    for (auto _ : state) {
        allocations += support::allocations_during([&] {
            for (std::int64_t k = 0; k < kBatch; ++k) {
                fuzz::apply(book, inputs[next++], out);
            }
        });
        benchmark::DoNotOptimize(out.size());
    }
    report(state, {.ops_per_batch = kBatch, .allocations = allocations});
}

} // namespace

// Every benchmark runs on the reference book, FastBook (sorted levels) and the TickFastBook
// experiment, so all three numbers come from identical code.
#define NANOBOOK_ALL_BOOKS(bench)                                                                  \
    BENCHMARK_TEMPLATE(bench, ReferenceBook)->Unit(benchmark::kMicrosecond);                       \
    BENCHMARK_TEMPLATE(bench, FastBook)->Unit(benchmark::kMicrosecond);                            \
    BENCHMARK_TEMPLATE(bench, TickFastBook)->Unit(benchmark::kMicrosecond)

#define NANOBOOK_ALL_BOOKS_FIXED(bench)                                                            \
    BENCHMARK_TEMPLATE(bench, ReferenceBook)                                                       \
        ->Iterations(kTimedBatches)                                                                \
        ->Unit(benchmark::kMicrosecond);                                                           \
    BENCHMARK_TEMPLATE(bench, FastBook)->Iterations(kTimedBatches)->Unit(benchmark::kMicrosecond); \
    BENCHMARK_TEMPLATE(bench, TickFastBook)                                                        \
        ->Iterations(kTimedBatches)                                                                \
        ->Unit(benchmark::kMicrosecond)

NANOBOOK_ALL_BOOKS(BM_AddResting);
NANOBOOK_ALL_BOOKS(BM_Cancel);
NANOBOOK_ALL_BOOKS(BM_MatchOne);
NANOBOOK_ALL_BOOKS(BM_Mixed);
// Sweeps use a fixed number of timed batches: a batch of 100 one-level sweeps is only ~2 µs of
// timed work, so letting Google Benchmark choose would mean hundreds of thousands of book rebuilds
// (untimed but slow), which pushed runs past a 5-minute limit (step 7).
#define NANOBOOK_SWEEPS(book)                                                                      \
    BENCHMARK_TEMPLATE(BM_Sweep, book)                                                             \
        ->Arg(1)                                                                                   \
        ->Arg(10)                                                                                  \
        ->Arg(100)                                                                                 \
        ->Iterations(kSweepBatches)                                                                \
        ->Unit(benchmark::kMicrosecond)

NANOBOOK_SWEEPS(ReferenceBook);
NANOBOOK_SWEEPS(FastBook);
NANOBOOK_SWEEPS(TickFastBook);
NANOBOOK_ALL_BOOKS_FIXED(BM_LargeCancel);
NANOBOOK_ALL_BOOKS_FIXED(BM_LargeAdd);
NANOBOOK_ALL_BOOKS_FIXED(BM_Aged);

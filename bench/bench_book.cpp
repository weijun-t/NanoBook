#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "alloc_counter.hpp"
#include "nanobook/core/event_buffer.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/reference/reference_book.hpp"

// Baseline benchmarks (design §13). Every benchmark is a template over the book type, so phase 2
// registers the fast book with the same code and the before/after numbers are comparable.
//
// What these numbers are, and aren't:
// - Google Benchmark reports AVERAGES over many operations, not tail latency (rule 17). The
//   p50/p99/p99.9 numbers come from the rdtsc harness in phase 3.
// - Measured on a laptop / WSL2 / CI, they are only for comparing books on the SAME machine.
//   Never publish them (rule 20). Record them in docs/perf-log.md with the environment (rule 19).

namespace {

using namespace nanobook;

constexpr std::uint64_t kSeed = 42; // fixed seed: the same input stream on every run (rule 11)
constexpr std::int64_t kBatch = 10'000;

// Room for every benchmark below. max_orders also sizes the event buffer (40 bytes per event).
constexpr BookConfig kConfig{Price{1}, Price{1'000'000}, 20'000, Qty{1'000'000}};

NewOrder order(OrderType type, Side side, std::uint32_t owner, std::uint64_t id, std::int64_t qty,
               std::int64_t price) {
    return NewOrder{.id = OrderId{id},
                    .owner = OwnerId{owner},
                    .side = side,
                    .type = type,
                    .price = Price{price},
                    .qty = Qty{qty}};
}

// The shape of every benchmark: each iteration builds a fresh book, runs `setup` on it with the
// timer paused, then times `work`, which performs `ops_per_batch` operations. Timing a batch
// instead of one operation keeps the cost of PauseTiming/ResumeTiming (well over 100 ns) small
// next to the work. Reports ns per operation and heap allocations per operation.
template <OrderBook Book, typename Setup, typename Work>
void run_batches(benchmark::State &state, std::int64_t ops_per_batch, Setup setup, Work work) {
    EventBuffer out{kConfig.max_events_per_input()}; // allocated once, reused
    // On the heap, so it can be replaced while paused. (std::optional<Book> works too, but GCC
    // -O3 gives a false -Wmaybe-uninitialized warning inside std::map for it.)
    std::unique_ptr<Book> book;
    std::uint64_t allocations = 0;

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<Book>(kConfig); // the previous book is destroyed here, untimed
        setup(*book, out);
        state.ResumeTiming();

        const std::uint64_t before = bench::allocations();
        work(*book, out);
        allocations += bench::allocations() - before;
        benchmark::DoNotOptimize(out.size()); // keep the compiler from discarding the work
    }

    const double total_ops =
        static_cast<double>(ops_per_batch) * static_cast<double>(state.iterations());
    state.SetItemsProcessed(static_cast<std::int64_t>(total_ops));
    // Rate inverted = seconds per operation; printed with a unit prefix (e.g. 250n = 250 ns).
    state.counters["time_per_op"] = benchmark::Counter(
        static_cast<double>(ops_per_batch),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["allocs_per_op"] = static_cast<double>(allocations) / total_ops;
}

// Adds kBatch buy orders spread over 100 price levels. Nothing to match, so every order rests.
template <OrderBook Book> void BM_AddResting(benchmark::State &state) {
    run_batches<Book>(
        state, kBatch, [](Book &, EventBuffer &) {},
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
        state, kBatch,
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
        state, kBatch,
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

    run_batches<Book>(
        state, kSweeps,
        [levels](Book &book, EventBuffer &out) {
            for (std::int64_t p = 1; p <= kSweeps * levels; ++p) { // prices 1, 2, 3, ...
                book.handle(
                    order(OrderType::Limit, Side::Sell, 2, static_cast<std::uint64_t>(p), 1, p),
                    out);
            }
        },
        [levels](Book &book, EventBuffer &out) {
            for (std::int64_t s = 0; s < kSweeps; ++s) {
                const auto id = static_cast<std::uint64_t>(kSweeps * levels + s + 1);
                // Takes the N cheapest levels that are left.
                book.handle(order(OrderType::Limit, Side::Buy, 1, id, levels, kSweeps * levels),
                            out);
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
        state, kBatch, [](Book &, EventBuffer &) {},
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

} // namespace

// Phase 2: register the fast book on each line below, e.g. BENCHMARK_TEMPLATE(BM_Cancel, FastBook).
BENCHMARK_TEMPLATE(BM_AddResting, ReferenceBook)->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_Cancel, ReferenceBook)->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_MatchOne, ReferenceBook)->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_Sweep, ReferenceBook)
    ->Arg(1)
    ->Arg(10)
    ->Arg(100)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_Mixed, ReferenceBook)->Unit(benchmark::kMicrosecond);

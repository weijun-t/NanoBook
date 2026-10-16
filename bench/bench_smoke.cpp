#include <benchmark/benchmark.h>
#include "nanobook/messages.hpp"

// Smoke test only: proves the benchmark binary builds and runs in CI. Not a measurement.
// Real benchmarks of the reference book arrive with the book itself (design §13).
static void EventCompareSmoke(benchmark::State &state) {
    using namespace nanobook;
    const Event a = Event::make_trade(OrderId{2}, OrderId{1}, Price{10}, Qty{40});
    Event b = a;
    for (auto _ : state) {
        benchmark::DoNotOptimize(b);
        benchmark::DoNotOptimize(a == b);
    }
}

BENCHMARK(EventCompareSmoke);

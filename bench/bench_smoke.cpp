#include <benchmark/benchmark.h>
#include "nanobook/core.hpp"

static void CoreBenchmark(benchmark::State &state) {
    for (auto _ : state) {
        nanobook::Core core;
        core.initialize();
        benchmark::DoNotOptimize(core);
    }
}

BENCHMARK(CoreBenchmark);
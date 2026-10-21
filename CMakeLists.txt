# CMakeLists.txt: how NanoBook is built.
#
# What it does: Builds the engine library (nanobook), the tests (nanobook_tests, plus
#   nanobook_alloc_tests in builds without sanitizers), the differential fuzzer (nanobook_fuzz),
#   the benchmarks (nanobook_microbench), the rdtsc latency harness (nanobook_bench), the ring
#   benchmark (nanobook_ringbench), the ITCH day replay (nanobook_replay), the pipeline
#   (nanobook_pipeline) and its log reader (nanobook_logdump). Downloads GoogleTest and Google
#   Benchmark at pinned versions, checked by SHA256.
#
# How it works: Options come from CMakePresets.json. Sanitizer flags are global, so third-party code
#   is built the same way as ours. The strict warnings (-Werror) apply to our code only. The engine
#   library is built with -fno-exceptions. The git commit is written into a generated header on
#   every build (cmake/git_commit.cmake), for the latency harness's environment block.

cmake_minimum_required(VERSION 3.25) # CMakePresets.json version 6 needs 3.25
project(nanobook LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 1. C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++23, not -std=gnu++23

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # compile_commands.json for clang-tidy / editors

# ---------------------------------------------------------------------------
# 2. Options (set by CMakePresets.json)
# ---------------------------------------------------------------------------
option(NANOBOOK_BUILD_TESTS "Build the test suite" OFF)
option(NANOBOOK_BUILD_BENCH "Build benchmarks" OFF)
option(NANOBOOK_NATIVE_OPTIMIZATION "Compile own targets with -march=native" OFF)
set(NANOBOOK_SANITIZER "" CACHE STRING "Comma-separated -fsanitize= list, e.g. address,undefined or thread")

# ---------------------------------------------------------------------------
# 3. Global flags. These must come BEFORE FetchContent so that GoogleTest and
#    Google Benchmark are built with the same flags as our code.
# ---------------------------------------------------------------------------
if(NANOBOOK_SANITIZER)
    if(NANOBOOK_SANITIZER MATCHES "address" AND NANOBOOK_SANITIZER MATCHES "thread")
        message(FATAL_ERROR "ASan and TSan cannot be combined; use separate presets")
    endif()
    message(STATUS "Sanitizers: ${NANOBOOK_SANITIZER}")
    # Every target in the program is instrumented and linked with the sanitizer runtime.
    # -fno-sanitize-recover=all: UBSan aborts (test fails) instead of printing and continuing.
    add_compile_options(-fsanitize=${NANOBOOK_SANITIZER} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${NANOBOOK_SANITIZER})
endif()

# perf needs frame pointers in every function to build call stacks / flame graphs.
add_compile_options($<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)

# ---------------------------------------------------------------------------
# 4. Per-target settings for OUR code only (never third-party code)
# ---------------------------------------------------------------------------
function(nanobook_configure_target target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
    if(NANOBOOK_NATIVE_OPTIMIZATION)
        target_compile_options(${target} PRIVATE -march=native)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 5. Core library
# ---------------------------------------------------------------------------
add_library(nanobook src/nanobook.cpp src/reference/reference_book.cpp src/fast/fast_book.cpp)
target_include_directories(nanobook PUBLIC include)
nanobook_configure_target(nanobook)
# Rule 3: the engine never throws. PRIVATE = only the library's own .cpp files;
# tests and benchmarks keep exceptions because GoogleTest relies on them.
target_compile_options(nanobook PRIVATE -fno-exceptions)

# ---------------------------------------------------------------------------
# 6. Test support (support/): headers shared by tests, tools and benchmarks, such as the
#    differential fuzzer. An INTERFACE library: it only adds the include directory.
# ---------------------------------------------------------------------------
add_library(nanobook_testing INTERFACE)
target_include_directories(nanobook_testing INTERFACE support)

# Allocation counter: replaces the global operator new to count calls. Shared by the
# benchmarks and the zero-allocation tests. An OBJECT library, so its object file is always
# linked in (a static library could be skipped by the linker). Never in sanitizer builds:
# ASan and TSan track allocations themselves.
if(NANOBOOK_BUILD_BENCH OR (NANOBOOK_BUILD_TESTS AND NOT NANOBOOK_SANITIZER))
    add_library(nanobook_alloc_counter OBJECT support/alloc_counter.cpp)
    target_link_libraries(nanobook_alloc_counter PUBLIC nanobook_testing)
    nanobook_configure_target(nanobook_alloc_counter)
endif()

# The git commit for the latency harness's environment block (latency-harness design §8):
# written once now (so a configure-only run, like CI's clang-tidy job, has the header) and again
# on every build by the target below, so it never goes stale. Build type and flags likewise.
if(NANOBOOK_BUILD_TESTS OR NANOBOOK_BUILD_BENCH)
    set(NANOBOOK_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
    set(NANOBOOK_GIT_HEADER "${NANOBOOK_GENERATED_DIR}/nanobook_git.hpp")
    file(MAKE_DIRECTORY "${NANOBOOK_GENERATED_DIR}")
    set(NANOBOOK_GIT_COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${NANOBOOK_GIT_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/git_commit.cmake)
    execute_process(COMMAND ${NANOBOOK_GIT_COMMAND})
    add_custom_target(nanobook_git_commit COMMAND ${NANOBOOK_GIT_COMMAND}
        BYPRODUCTS "${NANOBOOK_GIT_HEADER}" COMMENT "Recording the git commit")

    string(TOUPPER "${CMAKE_BUILD_TYPE}" NANOBOOK_BUILD_TYPE_UPPER)
    set(NANOBOOK_FLAGS_TEXT
        "-std=c++${CMAKE_CXX_STANDARD} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${NANOBOOK_BUILD_TYPE_UPPER}}")
    if(NANOBOOK_NATIVE_OPTIMIZATION)
        string(APPEND NANOBOOK_FLAGS_TEXT " -march=native")
    endif()
    string(REGEX REPLACE " +" " " NANOBOOK_FLAGS_TEXT "${NANOBOOK_FLAGS_TEXT}")
    string(STRIP "${NANOBOOK_FLAGS_TEXT}" NANOBOOK_FLAGS_TEXT)

    # Lets `target` include support/environment.hpp.
    function(nanobook_uses_environment target)
        add_dependencies(${target} nanobook_git_commit)
        target_include_directories(${target} PRIVATE "${NANOBOOK_GENERATED_DIR}")
        target_compile_definitions(${target} PRIVATE
            NANOBOOK_BUILD_TYPE="${CMAKE_BUILD_TYPE}" NANOBOOK_CXX_FLAGS="${NANOBOOK_FLAGS_TEXT}")
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 7. Tests (GoogleTest)
# ---------------------------------------------------------------------------
if(NANOBOOK_BUILD_TESTS OR NANOBOOK_BUILD_BENCH)
    include(FetchContent)
endif()

if(NANOBOOK_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)

    FetchContent_Declare(
        googletest
        URL      https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    )
    set(INSTALL_GTEST OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(googletest)

    add_executable(nanobook_tests
        tests/test_types.cpp
        tests/test_messages.cpp
        tests/test_book.cpp
        tests/test_exchange.cpp
        tests/test_fuzz.cpp
        tests/test_feed.cpp
        tests/test_latency.cpp
        tests/test_latency_scenarios.cpp
        tests/test_page_faults.cpp
        tests/fast/test_order_pool.cpp
        tests/fast/test_intrusive_queue.cpp
        tests/fast/test_id_table.cpp
        tests/fast/test_levels.cpp
        tests/fast/test_prefault_allocator.cpp
        tests/itch/test_parser.cpp
        tests/itch/test_book_builder.cpp
        tests/itch/test_replay_comparison.cpp
        tests/itch/test_itch_timing.cpp
        tests/itch/test_itch_flow.cpp
        tests/pipeline/test_spsc_ring.cpp
        tests/pipeline/test_pipeline.cpp
        tests/pipeline/test_binary_log.cpp
        tests/pipeline/test_pacing.cpp
    )
    target_link_libraries(nanobook_tests PRIVATE nanobook nanobook_testing GTest::gtest_main)
    nanobook_configure_target(nanobook_tests)
    nanobook_uses_environment(nanobook_tests)
    gtest_discover_tests(nanobook_tests)

    # Zero-allocation tests (README rule 1, fast-book design §8.4): their own program, because
    # the counting operator new would replace the normal one for every test in the program.
    if(NOT NANOBOOK_SANITIZER)
        add_executable(nanobook_alloc_tests tests/test_alloc.cpp)
        target_link_libraries(nanobook_alloc_tests PRIVATE nanobook nanobook_alloc_counter GTest::gtest_main)
        nanobook_configure_target(nanobook_alloc_tests)
        gtest_discover_tests(nanobook_alloc_tests)
    endif()

    # Differential fuzzer (fast-book design §8.3). Built in every preset, so it can also run
    # under the sanitizers.
    add_executable(nanobook_fuzz tools/nanobook_fuzz.cpp)
    target_link_libraries(nanobook_fuzz PRIVATE nanobook nanobook_testing)
    nanobook_configure_target(nanobook_fuzz)

    # ITCH day replay (ITCH design §10). Built in every preset so CI compiles it; running it
    # needs a day file, which isn't in the repo.
    add_executable(nanobook_replay tools/nanobook_replay.cpp)
    target_link_libraries(nanobook_replay PRIVATE nanobook nanobook_testing)
    nanobook_configure_target(nanobook_replay)
    nanobook_uses_environment(nanobook_replay)

    # The three-thread pipeline into a binary log, and the log back to CSV (pipeline design §6).
    add_executable(nanobook_pipeline tools/nanobook_pipeline.cpp)
    target_link_libraries(nanobook_pipeline PRIVATE nanobook nanobook_testing)
    nanobook_configure_target(nanobook_pipeline)
    nanobook_uses_environment(nanobook_pipeline)
    add_executable(nanobook_logdump tools/nanobook_logdump.cpp)
    target_link_libraries(nanobook_logdump PRIVATE nanobook nanobook_testing)
    nanobook_configure_target(nanobook_logdump)
endif()

# ---------------------------------------------------------------------------
# 8. Microbenchmarks (Google Benchmark). Named "microbench" so the name
#    nanobook_bench stays free for the rdtsc latency tool in tools/.
# ---------------------------------------------------------------------------
if(NANOBOOK_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE INTERNAL "")
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
    set(BENCHMARK_ENABLE_WERROR OFF CACHE INTERNAL "") # their warnings must not break our build

    FetchContent_Declare(
        benchmark
        URL      https://github.com/google/benchmark/archive/refs/tags/v1.8.5.tar.gz
        URL_HASH SHA256=d26789a2b46d8808a48a4556ee58ccc7c497fcd4c0af9b90197674a81e04798a
    )
    FetchContent_MakeAvailable(benchmark)

    add_executable(nanobook_microbench bench/bench_book.cpp)
    target_link_libraries(nanobook_microbench PRIVATE nanobook nanobook_alloc_counter benchmark::benchmark_main)
    nanobook_configure_target(nanobook_microbench)

    # The rdtsc latency harness (latency-harness design): times every operation one by one.
    add_executable(nanobook_bench tools/nanobook_bench.cpp)
    target_link_libraries(nanobook_bench PRIVATE nanobook nanobook_testing nanobook_alloc_counter)
    nanobook_configure_target(nanobook_bench)
    nanobook_uses_environment(nanobook_bench)

    # One SPSC ring between two pinned threads: throughput and round-trip latency (pipeline
    # design §11, step 1).
    add_executable(nanobook_ringbench tools/nanobook_ringbench.cpp)
    target_link_libraries(nanobook_ringbench PRIVATE nanobook nanobook_testing)
    nanobook_configure_target(nanobook_ringbench)
    nanobook_uses_environment(nanobook_ringbench)
endif()

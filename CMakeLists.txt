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
add_library(nanobook src/nanobook.cpp src/reference_book.cpp)
target_include_directories(nanobook PUBLIC include)
nanobook_configure_target(nanobook)
# Rule 3: the engine never throws. PRIVATE = only the library's own .cpp files;
# tests and benchmarks keep exceptions because GoogleTest relies on them.
target_compile_options(nanobook PRIVATE -fno-exceptions)

# ---------------------------------------------------------------------------
# 6. Tests (GoogleTest)
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
        tests/test_reference_book.cpp
        tests/test_exchange.cpp
    )
    target_link_libraries(nanobook_tests PRIVATE nanobook GTest::gtest_main)
    nanobook_configure_target(nanobook_tests)
    gtest_discover_tests(nanobook_tests)
endif()

# ---------------------------------------------------------------------------
# 7. Microbenchmarks (Google Benchmark). Named "microbench" so the name
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

    add_executable(nanobook_microbench bench/bench_smoke.cpp)
    target_link_libraries(nanobook_microbench PRIVATE nanobook benchmark::benchmark_main)
    nanobook_configure_target(nanobook_microbench)
endif()

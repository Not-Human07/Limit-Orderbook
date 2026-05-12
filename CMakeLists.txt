cmake_minimum_required(VERSION 3.20)
project(LimitOrderbook LANGUAGES CXX)

# Language standard
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # -std=c++20, not -std=gnu++20

# Default to Release if the user didn't specify.
# Never benchmark a Debug build — numbers are meaningless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "[LimitOrderbook] No build type specified — defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler flags
# Release  : -O3 -march=native  — full opt + native ISA (AVX2 on Ryzen 6600H)
# Debug    : -O0 -g + ASan/UBSan — catches the bugs flagged in the audit
set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -march=native -funroll-loops -fno-exceptions -DNDEBUG"
    CACHE STRING "Release flags" FORCE)

set(CMAKE_CXX_FLAGS_DEBUG
    "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    CACHE STRING "Debug flags" FORCE)

# Shared engine sources
# Compiled once, linked into both the demo and bench targets.
set(ENGINE_SOURCES
    src/orders.cpp
    src/order_book.cpp
    src/matching_engine.cpp
)

# demo  —  correctness / scenario target
# Run this first to verify the engine is correct before trusting bench numbers.
add_executable(demo
    demo/main.cpp
    ${ENGINE_SOURCES}
)
target_include_directories(demo PRIVATE include)

# bench  —  dedicated benchmark binary
# Always compiled with full optimisation regardless of CMAKE_BUILD_TYPE.
# Mixing -O0 engine code into a benchmark is a common mistake; this prevents it.

add_executable(benchmarks
    benchmarks/bench_main.cpp
    benchmarks/bench_throughput.cpp
    benchmarks/bench_cancel.cpp
    benchmarks/bench_latency.cpp
    ${ENGINE_SOURCES}
)
target_include_directories(benchmarks PRIVATE include)

# Force Release flags on the bench target unconditionally.
target_compile_options(benchmarks PRIVATE
    -O3
    -march=native
    -funroll-loops
    -fno-exceptions
    -DNDEBUG
)

# Optional: print a summary of what was configured
message(STATUS "---------------------------------------")
message(STATUS "  Project  : ${PROJECT_NAME}")
message(STATUS "  Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Targets  : demo, benchmarks")
message(STATUS "---------------------------------------")

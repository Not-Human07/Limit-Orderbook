cmake_minimum_required(VERSION 3.20)
project(LimitOrderbook LANGUAGES CXX)

# Language standard
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Default to Release if the user didn't specify.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "[LimitOrderbook] No build type specified — defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler flags — MSVC vs GCC/Clang
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /arch:AVX2 /DNDEBUG"
        CACHE STRING "Release flags" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG   "/Od /Zi /RTC1"
        CACHE STRING "Debug flags" FORCE)
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -funroll-loops -fno-exceptions -DNDEBUG"
        CACHE STRING "Release flags" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
        CACHE STRING "Debug flags" FORCE)
endif()

# Shared engine sources
set(ENGINE_SOURCES
    src/orders.cpp
    src/order_book.cpp
    src/matching_engine.cpp
)

# demo target
add_executable(demo
    demo/main.cpp
    ${ENGINE_SOURCES}
)
target_include_directories(demo PRIVATE include)

# benchmarks target
add_executable(benchmarks
    benchmarks/bench_main.cpp
    benchmarks/bench_throughput.cpp
    benchmarks/bench_cancel.cpp
    benchmarks/bench_latency.cpp
    ${ENGINE_SOURCES}
)
target_include_directories(benchmarks PRIVATE include)

# Force full optimization on benchmarks unconditionally
if(MSVC)
    target_compile_options(benchmarks PRIVATE
        /O2 /GL /arch:AVX2 /DNDEBUG
    )
    target_link_options(benchmarks PRIVATE /LTCG)
else()
    target_compile_options(benchmarks PRIVATE
        -O3 -march=native -funroll-loops -fno-exceptions -DNDEBUG
    )
endif()

message(STATUS "---------------------------------------")
message(STATUS "  Project  : ${PROJECT_NAME}")
message(STATUS "  Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Targets  : demo, benchmarks")
message(STATUS "---------------------------------------")

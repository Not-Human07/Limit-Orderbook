#pragma once

// bench_utils.hpp
// Shared utilities for all bench_*.cpp files.
// Included by bench_main.cpp only — never by engine code.
// Provides:
//   1. BenchResult     : holds raw numbers from one benchmark run
//   2. Timer           : RAII nanosecond wall-clock timer
//   3. print_header()  : consistent section separator
//   4. print_result()  : formatted table row

#include "matching_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>


// BenchResult
// Plain aggregate — each benchmark fills one of these and passes it to
// print_result() for consistent formatting.

struct BenchResult {
    std::string   name;
    std::uint64_t n           { 0 };      ///< Number of operations measured.
    double        elapsed_s   { 0.0 };    ///< Wall-clock seconds for the run.
    double        ops_per_sec { 0.0 };    ///< Derived: n / elapsed_s.
    double        ns_per_op   { 0.0 };    ///< Derived: elapsed_s * 1e9 / n.

    // Optional latency percentiles — filled by benchmarks that use LatencyStats.
    std::uint64_t lat_min  { 0 };
    std::uint64_t lat_p50  { 0 };
    std::uint64_t lat_p99  { 0 };
    std::uint64_t lat_p999 { 0 };
    std::uint64_t lat_max  { 0 };
    bool          has_latency { false };
};


// Timer
// Usage:
// Timer t;
//  work ...
//   double seconds = t.elapsed_s();

class Timer {
public:
    Timer() : t0_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] double elapsed_s() const noexcept {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0_).count();
    }

    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - t0_).count());
    }

    void reset() noexcept {
        t0_ = std::chrono::steady_clock::now();
    }

private:
    std::chrono::steady_clock::time_point t0_;
};


// Formatting helpers

inline void print_header(const std::string& title)
{
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(47) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
}

inline void print_result(const BenchResult& r)
{
    std::cout << std::fixed;

    std::cout << "\n  [" << r.name << "]\n";
    std::cout << "    operations : " << r.n << "\n";
    std::cout << "    elapsed    : " << std::setprecision(3) << r.elapsed_s  << " s\n";
    std::cout << "    throughput : " << std::setprecision(2) << r.ops_per_sec / 1e6 << " M ops/sec\n";
    std::cout << "    ns/op      : " << std::setprecision(1) << r.ns_per_op   << " ns\n";

    if (r.has_latency) {
        std::cout << "    latency    : "
                  << "min=" << r.lat_min
                  << "  p50=" << r.lat_p50
                  << "  p99=" << r.lat_p99
                  << "  p999=" << r.lat_p999
                  << "  max=" << r.lat_max
                  << "  ns\n";
    }
}

// Build a BenchResult from raw elapsed time.
inline BenchResult make_result(const std::string& name,
                                std::uint64_t      n,
                                double             elapsed_s)
{
    BenchResult r;
    r.name        = name;
    r.n           = n;
    r.elapsed_s   = elapsed_s;
    r.ops_per_sec = (elapsed_s > 0.0) ? (static_cast<double>(n) / elapsed_s) : 0.0;
    r.ns_per_op   = (n > 0)           ? (elapsed_s * 1e9 / static_cast<double>(n)) : 0.0;
    return r;
}

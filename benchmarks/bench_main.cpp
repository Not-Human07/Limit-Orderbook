#include "bench_utils.hpp"

// Forward declarations
// Each bench_*.cpp file defines its functions here.
// bench_main.cpp is the only translation unit with main().

// bench_throughput.cpp
BenchResult bench_throughput(std::uint64_t n);

// bench_cancel.cpp
BenchResult bench_cancel          (std::uint64_t n);
BenchResult bench_cancel_lifo     (std::uint64_t n);
BenchResult bench_cancel_scattered(std::uint64_t n);

// bench_latency.cpp
BenchResult bench_latency_resting (std::uint64_t n);
BenchResult bench_latency_matching(std::uint64_t n);
BenchResult bench_latency_ioc     (std::uint64_t n);
BenchResult bench_latency_fok_pass(std::uint64_t n);
BenchResult bench_latency_fok_fail(std::uint64_t n);

// Summary table
// After all benchmarks run, print a compact comparison table.
// This is the block you paste into the README.

static void print_summary(const std::vector<BenchResult>& results)
{
    const char* row_sep = "+--------------------------------------------------------------------------+\n";
    const char* col_sep = "+--------------------------------------------------------------------------+\n";

    std::cout << "\n";
    std::cout << row_sep;
    std::cout << "|                         BENCHMARK SUMMARY                               |\n";
    std::cout << "|  " << std::left
              << std::setw(38) << "Benchmark"
              << std::setw(14) << "M ops/sec"
              << std::setw(10) << "ns/op"
              << "  |\n";
    std::cout << col_sep;

    for (const auto& r : results) {
        std::cout << "|  " << std::left
                  << std::setw(38) << r.name
                  << std::fixed << std::setprecision(2)
                  << std::setw(14) << (r.ops_per_sec / 1e6)
                  << std::setprecision(1)
                  << std::setw(10) << r.ns_per_op
                  << "  |\n";
    }

    std::cout << row_sep;

    // Latency breakdown for benchmarks that have it.
    bool printed_header = false;
    for (const auto& r : results) {
        if (!r.has_latency) continue;
        if (!printed_header) {
            std::cout << "\nLatency percentiles (ns):\n";
            std::cout << std::left
                      << std::setw(40) << "  Benchmark"
                      << std::setw(8)  << "min"
                      << std::setw(8)  << "p50"
                      << std::setw(8)  << "p99"
                      << std::setw(8)  << "p99.9"
                      << std::setw(8)  << "max"
                      << "\n";
            printed_header = true;
        }
        std::cout << std::left
                  << std::setw(40) << ("  " + r.name)
                  << std::setw(8)  << r.lat_min
                  << std::setw(8)  << r.lat_p50
                  << std::setw(8)  << r.lat_p99
                  << std::setw(8)  << r.lat_p999
                  << std::setw(8)  << r.lat_max
                  << "\n";
    }
}

// main
int main()
{
    std::cout << "\n";
    std::cout << "=======================================================\n";
    std::cout << "  Limit Order Book Engine -- Benchmark Suite\n";
    std::cout << "  Build: Release  (-O3 -march=native)\n";
    std::cout << "=======================================================\n";

    std::vector<BenchResult> all_results;

    // Throughput 
    print_header("1. Throughput - rest + aggressive match alternating");
    {
        auto r = bench_throughput(300'000);
        print_result(r);
        all_results.push_back(r);
    }

    // Cancel 
    print_header("2. Cancel latency - FIFO, LIFO, scattered");
    {
        auto r1 = bench_cancel(50'000);
        auto r2 = bench_cancel_lifo(50'000);
        auto r3 = bench_cancel_scattered(50'000);
        print_result(r1);
        print_result(r2);
        print_result(r3);
        all_results.push_back(r1);
        all_results.push_back(r2);
        all_results.push_back(r3);
    }

    // Latency by path
    print_header("3. Per-path latency distribution");
    {
        auto r1 = bench_latency_resting (100'000);
        auto r2 = bench_latency_matching(100'000);
        auto r3 = bench_latency_ioc     (100'000);
        auto r4 = bench_latency_fok_pass( 50'000);
        auto r5 = bench_latency_fok_fail( 50'000);
        print_result(r1);
        print_result(r2);
        print_result(r3);
        print_result(r4);
        print_result(r5);
        all_results.push_back(r1);
        all_results.push_back(r2);
        all_results.push_back(r3);
        all_results.push_back(r4);
        all_results.push_back(r5);
    }

    // Summary 
    print_summary(all_results);

    std::cout << "\n  Done. Paste the summary block into README.md.\n\n";
    return 0;
}

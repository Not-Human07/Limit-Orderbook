#include "bench_utils.hpp"

#include <random>
// bench_throughput
// Measures raw order submission throughput under matching pressure.
// Pattern: alternating rest / aggress
//   even i → resting bid  at 100.00  (goes into the book)
//   odd  i → aggressive sell at 100.00  (crosses and fills the bid)
// Every other order causes a match.  This is the worst case for the matching
// path and the most representative of a live crossed book.
// Returns a BenchResult with latency percentiles populated from the book's
// internal LatencyStats ring buffer.

BenchResult bench_throughput(std::uint64_t n = 300'000)
{
    MatchingEngine eng;
    eng.add_symbol("T");

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    // Warmup
    // Pool, icache, and branch predictor all need warming before the timed
    // region.  10% of n is enough; we reset stats immediately after.
    const std::uint64_t warmup = n / 10;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        eng.submit_limit("T", s, PRICE(100.00), 1);
    }
    eng.reset_stats();

    // Timed region
    Timer t;
    for (std::uint64_t i = 0; i < n; ++i) {
        if (i % 2 == 0)
            eng.submit_limit("T", Side::Buy,  PRICE(100.00), qty_dist(rng));
        else
            eng.submit_limit("T", Side::Sell, PRICE(100.00), qty_dist(rng));
    }
    const double elapsed = t.elapsed_s();

    // Latency percentiles from book ring buffer
    BenchResult r = make_result("throughput (rest+match alternating)", n, elapsed);

    if (const OrderBook* bk = eng.book("T")) {
        auto p       = bk->latency_stats().compute();
        r.lat_min    = p.min;
        r.lat_p50    = p.p50;
        r.lat_p99    = p.p99;
        r.lat_p999   = p.p999;
        r.lat_max    = p.max;
        r.has_latency = true;
    }

    return r;
}

#include "bench_utils.hpp"

#include <numeric>
#include <vector>

// bench_cancel
// Measures the latency of cancel_order in isolation.

// Setup phase (untimed):
//   Place n resting bids at the same price level.  All share one PriceLevel
//   node in the map.  This is the hot case — a deep queue at a single price.

// Timed phase:
//   Cancel every order in submission order (FIFO).
//   Each cancel = OrderIndex lookup + intrusive unlink + pool dealloc.
//   Expected: ~30–40 ns on a warm cache.
// Also runs a second pass cancelling in reverse order (LIFO / tail cancel)
// to show that the intrusive list is O(1) regardless of position.

BenchResult bench_cancel(std::uint64_t n = 50'000)
{
    // Setup: place all orders, collect their ids
    MatchingEngine eng;
    eng.add_symbol("C");

    std::vector<OrderId> ids;
    ids.reserve(n);

    for (std::uint64_t i = 0; i < n; ++i) {
        auto r = eng.submit_limit("C", Side::Buy, PRICE(100.00), 1);
        ids.push_back(r.order_id);
    }

    // Timed: FIFO cancel (head → tail)
    Timer t;
    for (OrderId id : ids)
        eng.cancel_order("C", id);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("cancel FIFO (head→tail)", n, elapsed);
    return r;
}

BenchResult bench_cancel_lifo(std::uint64_t n = 50'000)
{
    MatchingEngine eng;
    eng.add_symbol("C");

    std::vector<OrderId> ids;
    ids.reserve(n);

    for (std::uint64_t i = 0; i < n; ++i) {
        auto r = eng.submit_limit("C", Side::Buy, PRICE(100.00), 1);
        ids.push_back(r.order_id);
    }

    // Reverse, cancel tail first.
    std::reverse(ids.begin(), ids.end());

    Timer t;
    for (OrderId id : ids)
        eng.cancel_order("C", id);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("cancel LIFO (tail→head)", n, elapsed);
    return r;
}

// Scattered cancel, cancels at random positions across many price levels.
// This stresses the OrderIndex hash map more than the intrusive list.

BenchResult bench_cancel_scattered(std::uint64_t n = 50'000)
{
    MatchingEngine eng;
    eng.add_symbol("C");

    // Spread across 500 price levels, ~100 orders per level.
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<Price> price_dist(PRICE(95.00), PRICE(99.99));

    std::vector<OrderId> ids;
    ids.reserve(n);

    for (std::uint64_t i = 0; i < n; ++i) {
        auto r = eng.submit_limit("C", Side::Buy, price_dist(rng), 1);
        ids.push_back(r.order_id);
    }

    // Shuffle cancel order to maximise cache miss pressure on OrderIndex.
    std::shuffle(ids.begin(), ids.end(), rng);

    Timer t;
    for (OrderId id : ids)
        eng.cancel_order("C", id);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("cancel scattered (random price levels)", n, elapsed);
    return r;
}

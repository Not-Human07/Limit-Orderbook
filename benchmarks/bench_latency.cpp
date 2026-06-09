#include "bench_utils.hpp"

#include <random>

// bench_latency
// Measures per-order latency distribution broken down by order path:
//   1. Resting path : order submitted, no match, rests in book
//   2. Matching path : order submitted, fully fills an existing resting order
//   3. IOC path : order fills what it can, remainder cancelled
//   4. FOK pass path : order fills fully (pre-check passes)
//   5. FOK fail path : order rejected (pre-check fails, book untouched)

// Each path has a different cost profile.  Separating them tells you exactly
// where latency comes from — useful for knowing which path to optimise next.

// Latency is read from the OrderBook's internal LatencyStats ring buffer
// which records every add_order call in nanoseconds.

// Helper: extract percentiles from a book and populate a BenchResult.
static void attach_latency(BenchResult& r, const OrderBook* bk)
{
    if (!bk) return;
    auto p       = bk->latency_stats().compute();
    r.lat_min    = p.min;
    r.lat_p50    = p.p50;
    r.lat_p99    = p.p99;
    r.lat_p999   = p.p999;
    r.lat_max    = p.max;
    r.has_latency = true;
}

// 1. Resting path
// Orders rest at non-crossing prices — no match occurs.
// Measures: pool alloc + level map insert + OrderIndex insert.

BenchResult bench_latency_resting(std::uint64_t n = 100'000)
{
    MatchingEngine eng;
    const SymbolId sym = eng.add_symbol("L");

    std::mt19937_64 rng(1);
    // Spread across 200 bid levels well below any ask.
    std::uniform_int_distribution<Price> px(PRICE(90.00), PRICE(94.99));

    // Warmup.
    for (std::uint64_t i = 0; i < n / 10; ++i)
        eng.submit_limit(sym, Side::Buy, px(rng), 1);
    // Reset so warmup doesn't pollute the ring buffer.
    // We need a fresh book for a clean ring buffer.
    MatchingEngine eng2;
    const SymbolId sym2 = eng2.add_symbol("L");

    Timer t;
    for (std::uint64_t i = 0; i < n; ++i)
        eng2.submit_limit(sym2, Side::Buy, px(rng), 1);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("latency: resting (no match)", n, elapsed);
    attach_latency(r, eng2.book(sym2));
    return r;
}

// 2. Matching path
// Every order fully matches an existing resting order.
// Measures: pool alloc + level map begin() + intrusive unlink + pool dealloc.

BenchResult bench_latency_matching(std::uint64_t n = 100'000)
{
    MatchingEngine eng;
    const SymbolId sym = eng.add_symbol("L");

    // Pre-load resting asks, one per order we'll send.
    // Use a fixed price so every aggressor crosses immediately.
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_limit(sym, Side::Sell, PRICE(100.00), 1);

    eng.reset_stats();

    // Warmup pass — separate engine.
    {
        MatchingEngine w;
        const SymbolId wsym = w.add_symbol("L");
        for (std::uint64_t i = 0; i < n / 10; ++i)
            w.submit_limit(wsym, Side::Sell, PRICE(100.00), 1);
        for (std::uint64_t i = 0; i < n / 10; ++i)
            w.submit_limit(wsym, Side::Buy,  PRICE(100.00), 1);
    }

    // Now time the matching path on the pre-loaded book.
    Timer t;
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_limit(sym, Side::Buy, PRICE(100.00), 1);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("latency: matching (every order fills)", n, elapsed);
    attach_latency(r, eng.book(sym));
    return r;
}

// 3. IOC path
// IOC: partial fill + remainder cancelled.  No resting in book.
// Measures: match pass + IOC cancel of remainder.

BenchResult bench_latency_ioc(std::uint64_t n = 100'000)
{
    MatchingEngine eng;
    const SymbolId sym = eng.add_symbol("L");

    std::mt19937_64 rng(3);
    std::uniform_int_distribution<Quantity> qty(1, 5);

    // Thin ask liquidity — IOC will partially fill and cancel the rest.
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_limit(sym, Side::Sell, PRICE(100.00), qty(rng));

    eng.reset_stats();

    Timer t;
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_ioc(sym, Side::Buy, PRICE(100.00), 10);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("latency: IOC (partial fill + cancel)", n, elapsed);
    attach_latency(r, eng.book(sym));
    return r;
}

// 4. FOK pass path
// FOK that fills fully: simulate pass succeeds, real match runs.
// Measures: simulate pass + real match pass (2× work vs GTC).

BenchResult bench_latency_fok_pass(std::uint64_t n = 50'000)
{
    MatchingEngine eng;
    const SymbolId sym = eng.add_symbol("L");

    // Load enough asks to fill every FOK.
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_limit(sym, Side::Sell, PRICE(100.00), 10);

    eng.reset_stats();

    Timer t;
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_fok(sym, Side::Buy, PRICE(100.00), 10);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("latency: FOK pass (simulate + real fill)", n, elapsed);
    attach_latency(r, eng.book(sym));
    return r;
}

// 5. FOK fail path
// FOK that cannot fill: simulate pass fails, order rejected.
// Book is never touched.  Measures: simulate pass only.

BenchResult bench_latency_fok_fail(std::uint64_t n = 50'000)
{
    MatchingEngine eng;
    const SymbolId sym = eng.add_symbol("L");

    // Only 1 unit of liquidity, FOK for 1000 will always fail.
    eng.submit_limit(sym, Side::Sell, PRICE(100.00), 1);
    eng.reset_stats();

    Timer t;
    for (std::uint64_t i = 0; i < n; ++i)
        eng.submit_fok(sym, Side::Buy, PRICE(100.00), 1000);
    const double elapsed = t.elapsed_s();

    BenchResult r = make_result("latency: FOK fail (simulate, reject, no fill)", n, elapsed);
    attach_latency(r, eng.book(sym));
    return r;
}

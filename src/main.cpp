#include "matching_engine.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>


// Display helpers
// Horizontal rule for section separation.
static void section(const std::string& title)
{
    std::cout << "\n┌─────────────────────────────────────────\n"
              << "│  " << title << "\n"
              << "└─────────────────────────────────────────\n";
}

// Print an OrderResult in a consistent, readable format.
// Uses price_to_double() for human-readable prices — all internal arithmetic
// remains in integer ticks.
static void print_result(const std::string& label, const OrderResult& r)
{
    std::cout << "  [" << std::left << std::setw(28) << label << "]"
              << "  id=" << std::setw(4) << r.order_id;

    if (!r.accepted) {
        std::cout << "  ✗ REJECTED: " << r.reject_reason << "\n";
        return;
    }

    if (r.trades.empty()) {
        std::cout << "  resting (no immediate fill)\n";
    } else {
        std::cout << "  ✓ " << r.trades.size() << " fill(s)";
        Quantity total = 0;
        for (const auto& t : r.trades) total += t.quantity;
        std::cout << "  total_qty=" << total << "\n";
        for (const auto& t : r.trades) {
            std::cout << "      trade #" << t.trade_seq
                      << "  price=" << std::fixed << std::setprecision(4)
                      << price_to_double(t.price)
                      << "  qty=" << t.quantity
                      << "  buy=" << t.buy_order_id
                      << "  sell=" << t.sell_order_id
                      << "\n";
        }
    }
}

// Scenario 1 — Basic limit match (price-time priority)
// Two bids rest at different prices.  A crossing sell aggresses and should
// match the better-priced (102.00) bid first.
static void demo_basic_limit_match(MatchingEngine& eng)
{
    section("1. Basic limit match — price-time priority");
    const std::string sym = "AAPL";

    // Bids rest in the book.
    auto r1 = eng.submit_limit(sym, Side::Buy,  PRICE(102.00), 10);
    auto r2 = eng.submit_limit(sym, Side::Buy,  PRICE(101.50), 20);

    print_result("BUY  LIMIT 102.00 x10", r1);
    print_result("BUY  LIMIT 101.50 x20", r2);

    std::cout << "\n  Book before aggressor:\n";
    eng.print_book(sym, 5);

    // Aggressive sell crosses both levels (qty=10 matches the 102.00 bid fully).
    auto r3 = eng.submit_limit(sym, Side::Sell, PRICE(101.00), 10);
    print_result("SELL LIMIT 101.00 x10", r3);

    std::cout << "\n  Book after aggressor:\n";
    eng.print_book(sym, 5);
}

// ============================================================================
// Scenario 2 — Partial fill
//
// Aggressor wants more than what's available at the crossing level.
// Remainder rests in the book as a GTC order.
// ============================================================================
static void demo_partial_fill(MatchingEngine& eng)
{
    section("2. Partial fill — aggressor larger than resting liquidity");
    const std::string sym = "AAPL";

    // Thin ask: only 5 available at 105.
    auto r1 = eng.submit_limit(sym, Side::Sell, PRICE(105.00), 5);
    print_result("SELL LIMIT 105.00 x5 ", r1);

    // Buy for 20 — fills 5, rests 15 at 105.
    auto r2 = eng.submit_limit(sym, Side::Buy,  PRICE(105.00), 20);
    print_result("BUY  LIMIT 105.00 x20", r2);

    std::cout << "\n  Book (20 - 5 = 15 should rest on bid at 105):\n";
    eng.print_book(sym, 5);
}


// Scenario 3 — Cancellation
// Place an order, cancel it, then verify a second cancel returns false.
static void demo_cancel(MatchingEngine& eng)
{
    section("3. Cancellation — single and double-cancel");
    const std::string sym = "AAPL";

    auto r1 = eng.submit_limit(sym, Side::Buy, PRICE(99.00), 50);
    print_result("BUY  LIMIT  99.00 x50", r1);

    bool ok = eng.cancel_order(sym, r1.order_id);
    std::cout << "  cancel id=" << r1.order_id
              << (ok ? "  -> ✓ cancelled\n" : "  -> ✗ not found\n");

    bool again = eng.cancel_order(sym, r1.order_id);
    std::cout << "  cancel id=" << r1.order_id << " (again)"
              << (again ? "  -> ✗ unexpected success\n"
                        : "  -> ✓ correctly not found\n");

    std::cout << "\n  Book (should be empty or unchanged):\n";
    eng.print_book(sym, 5);
}

// Scenario 4 — Market order sweeps multiple price levels
// Build an ask ladder at three levels then sweep with a market buy that
// exhausts two levels fully and partially fills the third.
static void demo_market_sweep(MatchingEngine& eng)
{
    section("4. Market order sweeping multiple price levels");
    const std::string sym = "TSLA";

    eng.submit_limit(sym, Side::Sell, PRICE(200.00), 10);
    eng.submit_limit(sym, Side::Sell, PRICE(201.00), 10);
    eng.submit_limit(sym, Side::Sell, PRICE(202.00), 10);

    std::cout << "\n  Ask ladder before market order:\n";
    eng.print_book(sym, 5);

    // Market buy for 25: fills 200 (10) + 201 (10) fully, 202 partial (5).
    auto r = eng.submit_market(sym, Side::Buy, 25);
    print_result("MKT  BUY         x25 ", r);

    std::cout << "\n  Book after sweep (5 should remain at 202):\n";
    eng.print_book(sym, 5);
}

// Scenario 5 — IOC: fills what it can, cancels remainder immediately
static void demo_ioc(MatchingEngine& eng)
{
    section("5. IOC — immediate-or-cancel partial fill");
    const std::string sym = "TSLA";

    // Only 3 available at ask.
    eng.submit_limit(sym, Side::Sell, PRICE(210.00), 3);

    // IOC buy for 10 at 210 — fills 3, cancels remaining 7 (never rests).
    auto r = eng.submit_ioc(sym, Side::Buy, PRICE(210.00), 10);
    print_result("BUY  IOC   210.00 x10", r);

    std::cout << "\n  Book (IOC remainder must NOT rest — bid side empty):\n";
    eng.print_book(sym, 5);
}

// Scenario 6 — FOK: reject if can't fill entirely
static void demo_fok(MatchingEngine& eng)
{
    section("6. FOK — fill-or-kill, reject on insufficient liquidity");
    const std::string sym = "TSLA";

    // Only 5 available — FOK for 20 must be rejected outright.
    eng.submit_limit(sym, Side::Sell, PRICE(215.00), 5);

    auto r_reject = eng.submit_fok(sym, Side::Buy, PRICE(215.00), 20);
    print_result("BUY  FOK   215.00 x20", r_reject);

    std::cout << "  (book should be unchanged — FOK never touched it)\n";
    eng.print_book(sym, 5);

    // Now FOK for exactly 5 — should fill fully.
    auto r_fill = eng.submit_fok(sym, Side::Buy, PRICE(215.00), 5);
    print_result("BUY  FOK   215.00 x5 ", r_fill);
}

// Scenario 7 — Unknown symbol rejection
static void demo_reject_unknown(MatchingEngine& eng)
{
    section("7. Reject — unregistered symbol");

    auto r = eng.submit_limit("UNKNOWN", Side::Buy, PRICE(50.00), 1);
    print_result("BUY  LIMIT  50.00 x1 ", r);
}

// Scenario 8 — Multi-level depth snapshot
// Build a realistic order book with multiple levels on each side and
// print the depth ladder.

static void demo_depth_snapshot(MatchingEngine& eng)
{
    section("8. Depth snapshot — multi-level book");
    const std::string sym = "NVDA";

    // Bids: descending prices, various sizes.
    eng.submit_limit(sym, Side::Buy,  PRICE(500.00), 100);
    eng.submit_limit(sym, Side::Buy,  PRICE(499.50),  80);
    eng.submit_limit(sym, Side::Buy,  PRICE(499.00),  60);
    eng.submit_limit(sym, Side::Buy,  PRICE(498.50),  40);
    eng.submit_limit(sym, Side::Buy,  PRICE(498.00),  20);

    // Asks: ascending prices.
    eng.submit_limit(sym, Side::Sell, PRICE(500.50),  50);
    eng.submit_limit(sym, Side::Sell, PRICE(501.00),  70);
    eng.submit_limit(sym, Side::Sell, PRICE(501.50),  90);
    eng.submit_limit(sym, Side::Sell, PRICE(502.00), 110);
    eng.submit_limit(sym, Side::Sell, PRICE(502.50), 130);

    eng.print_book(sym, 5);

    // BBO and spread.
    if (auto bid = eng.best_bid(sym))
        std::cout << "  best_bid  = " << std::fixed << std::setprecision(4)
                  << price_to_double(*bid) << "\n";
    if (auto ask = eng.best_ask(sym))
        std::cout << "  best_ask  = " << price_to_double(*ask) << "\n";
    if (auto sp = eng.spread(sym))
        std::cout << "  spread    = " << price_to_double(*sp) << " (ticks=" << *sp << ")\n";
    if (auto mp = eng.mid_price(sym))
        std::cout << "  mid_price = " << price_to_double(*mp) << "\n";
}

// Benchmark — throughput and latency
// Isolated engine with no I/O callbacks.  Alternates resting bids and
// aggressive sells to force matching on every other order, which stresses
// both the resting path and the matching path equally.

static void run_benchmark(std::uint64_t num_orders = 300'000)
{
    section("Benchmark — throughput & latency");

    MatchingEngine eng;   // No callbacks — zero I/O in hot path.
    eng.add_symbol("BENCH");

    // Use a fixed seed so results are reproducible across runs.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price>    price_dist(PRICE(99.00), PRICE(101.00));
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    std::cout << "  Warming up (" << num_orders / 10 << " orders)...\n";

    // Warmup — fills the pool and warms the icache.
    const std::uint64_t warmup = num_orders / 10;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        eng.submit_limit("BENCH", s, PRICE(100.00), 1);
    }
    eng.reset_stats();

    // Timed run.
    const auto t0 = std::chrono::steady_clock::now();

    for (std::uint64_t i = 0; i < num_orders; ++i) {
        // Alternate: even = resting bid, odd = aggressive sell that crosses.
        if (i % 2 == 0) {
            eng.submit_limit("BENCH", Side::Buy,  price_dist(rng), qty_dist(rng));
        } else {
            eng.submit_limit("BENCH", Side::Sell, PRICE(99.00), qty_dist(rng));
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(t1 - t0).count();

    const double mops = static_cast<double>(num_orders) / elapsed_s / 1e6;

    std::cout << "\n  [throughput]  "
              << num_orders << " orders in "
              << std::fixed << std::setprecision(3) << elapsed_s << "s"
              << "  →  " << std::setprecision(2) << mops << " M orders/sec\n";

    // Per-order latency from the book's ring buffer.
    const auto& bk = eng.book("BENCH");
    if (bk) {
        auto perc = bk->latency_stats().compute();
        std::cout << "  [latency ns]  "
                  << "min="  << perc.min
                  << "  p50=" << perc.p50
                  << "  p99=" << perc.p99
                  << "  p999=" << perc.p999
                  << "  max=" << perc.max << "\n";
    }

    // Cancel latency — 50K cancels on resting orders.
    std::cout << "\n  Running cancel benchmark (50 000 cancels)...\n";
    {
        MatchingEngine ceng;
        ceng.add_symbol("CXLBENCH");

        std::vector<OrderId> ids;
        ids.reserve(50'000);

        // Place 50K resting bids.
        for (std::uint64_t i = 0; i < 50'000; ++i) {
            auto r = ceng.submit_limit("CXLBENCH", Side::Buy, PRICE(100.00), 1);
            ids.push_back(r.order_id);
        }

        const auto c0 = std::chrono::steady_clock::now();
        for (OrderId id : ids)
            ceng.cancel_order("CXLBENCH", id);
        const auto c1 = std::chrono::steady_clock::now();

        const double cancel_ns =
            std::chrono::duration<double, std::nano>(c1 - c0).count();
        std::cout << "  [cancel]      50 000 cancels"
                  << "  →  "
                  << std::fixed << std::setprecision(1)
                  << cancel_ns / 50'000.0 << " ns avg\n";
    }
}

// main
int main()
{
    std::cout << std::fixed << std::setprecision(4);

    // Engine-level callbacks — fired for every fill and every rejection.
    auto on_trade = [](const std::string& sym, const Trade& t) {
        std::cout << "  [FILL]   " << sym
                  << "  #" << t.trade_seq
                  << "  price=" << std::fixed << std::setprecision(4)
                  << price_to_double(t.price)
                  << "  qty=" << t.quantity
                  << "  buy=" << t.buy_order_id
                  << "  sell=" << t.sell_order_id
                  << "\n";
    };

    auto on_reject = [](OrderId id, const std::string& reason) {
        std::cout << "  [REJECT] id=" << id << "  " << reason << "\n";
    };

    MatchingEngine eng(on_trade, on_reject);
    eng.add_symbol("AAPL");
    eng.add_symbol("TSLA");
    eng.add_symbol("NVDA");

    // Demo scenarios 
    demo_basic_limit_match(eng);
    demo_partial_fill(eng);
    demo_cancel(eng);
    demo_market_sweep(eng);
    demo_ioc(eng);
    demo_fok(eng);
    demo_reject_unknown(eng);
    demo_depth_snapshot(eng);

    // Summary stats from the demo run 
    std::cout << "\n";
    eng.print_stats();

    run_benchmark(300'000);
    return 0;
}

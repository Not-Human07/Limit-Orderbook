#include "matching_engine.hpp"

#include <iostream>
#include <iomanip>
#include <string>

// Helpers

static void print_result(const std::string& label, const OrderResult& r)
{
    std::cout << "  [" << label << "] order_id=" << r.order_id;

    if (!r.accepted) {
        std::cout << "  REJECTED: " << r.reject_reason << "\n";
        return;
    }

    if (r.trades.empty()) {
        std::cout << "  resting (no immediate fill)\n";
    } else {
        std::cout << "  -> " << r.trades.size() << " trade(s):\n";
        for (auto& t : r.trades)
            std::cout << "       " << t.to_string() << "\n";
    }
}

static void section(const std::string& title)
{
    std::cout << "\n┌─ " << title << "\n";
}

// Scenarios

// Basic limit order resting and a crossing order that matches it.
static void demo_basic_limit_match(MatchingEngine& eng)
{
    section("Basic limit match");
    const std::string sym = "AAPL";

    // Two bids resting — price-time priority means the 102 bid fills first.
    auto r1 = eng.submit_limit(sym, Side::Buy,  102.00, 10, "alice");
    auto r2 = eng.submit_limit(sym, Side::Buy,  101.50, 20, "bob");
    auto r3 = eng.submit_limit(sym, Side::Sell, 101.00, 10, "carol"); // crosses both levels

    print_result("BUY  102.00 x10  alice", r1);
    print_result("BUY  101.50 x20  bob  ", r2);
    print_result("SELL 101.00 x10  carol", r3);

    eng.print_book(sym);
}

// Partial fill — aggressor is larger than resting quantity.
static void demo_partial_fill(MatchingEngine& eng)
{
    section("Partial fill");
    const std::string sym = "AAPL";

    auto r1 = eng.submit_limit(sym, Side::Sell, 105.00, 5,  "dave");
    auto r2 = eng.submit_limit(sym, Side::Buy,  105.00, 20, "eve");  // wants 20, only 5 available

    print_result("SELL 105.00 x5   dave", r1);
    print_result("BUY  105.00 x20  eve ", r2);

    eng.print_book(sym);
}

// Order cancellation — cancel before it gets hit.
static void demo_cancel(MatchingEngine& eng)
{
    section("Cancellation");
    const std::string sym = "AAPL";

    auto r1 = eng.submit_limit(sym, Side::Buy, 99.00, 50, "frank");
    print_result("BUY  99.00 x50  frank", r1);

    bool ok = eng.cancel_order(sym, r1.order_id);
    std::cout << "  cancel order_id=" << r1.order_id
              << (ok ? "  -> cancelled\n" : "  -> not found\n");

    // Try cancelling a second time — should return false.
    bool again = eng.cancel_order(sym, r1.order_id);
    std::cout << "  cancel again      -> "
              << (again ? "cancelled (unexpected!)\n" : "not found (correct)\n");

    eng.print_book(sym);
}

// Market order sweeps multiple price levels.
static void demo_market_sweep(MatchingEngine& eng)
{
    section("Market order sweeping multiple levels");
    const std::string sym = "TSLA";

    // Build an ask ladder.
    eng.submit_limit(sym, Side::Sell, 200.00, 10, "mm");
    eng.submit_limit(sym, Side::Sell, 201.00, 10, "mm");
    eng.submit_limit(sym, Side::Sell, 202.00, 10, "mm");

    std::cout << "  Book before market order:\n";
    eng.print_book(sym, 5);

    // Market buy for 25 — clears the 200 and 201 levels fully, partial on 202.
    auto r = eng.submit_market(sym, Side::Buy, 25, "hedge_fund");
    print_result("MKT BUY x25  hedge_fund", r);

    eng.print_book(sym);
}

// Reject path — unknown symbol.
static void demo_reject(MatchingEngine& eng)
{
    section("Reject: unknown symbol");

    auto r = eng.submit_limit("UNKNOWN", Side::Buy, 50.00, 1, "ghost");
    print_result("BUY  50.00 x1  ghost", r);
}

// Main

int main()
{
    std::cout << std::fixed << std::setprecision(2);

    // Trade callback — just prints every fill as it happens.
    auto on_trade = [](const std::string& symbol, const Trade& t) {
        std::cout << "  [FILL] " << symbol << " " << t.to_string() << "\n";
    };

    auto on_reject = [](OrderId id, const std::string& reason) {
        std::cout << "  [REJECT] order_id=" << id << " reason=" << reason << "\n";
    };

    MatchingEngine eng(on_trade, on_reject);
    eng.add_symbol("AAPL");
    eng.add_symbol("TSLA");

    // ── Run demo scenarios ────────────────────────────────────────────────

    demo_basic_limit_match(eng);
    demo_partial_fill(eng);
    demo_cancel(eng);
    demo_market_sweep(eng);
    demo_reject(eng);

    // Stats from the demo run

    std::cout << "\n";
    eng.print_stats();

    // Benchmark
    //
    // Fresh engine — no callbacks, no cout overhead in the hot path.

    std::cout << "\n";
    MatchingEngine bench_eng;
    bench_eng.run_benchmark("BENCH", 1'000'000);

    return 0;
}

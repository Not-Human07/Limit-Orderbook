#include "order_book.hpp"
#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

// LatencyStats
LatencyStats::Percentiles LatencyStats::compute() const
{
    if (count == 0) return {};
    std::vector<std::uint64_t> sorted(samples.begin(),
                                       samples.begin() + count);
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
        return sorted[idx];
    };
    return {
        .min  = sorted.front(),
        .p50  = pct(0.50),
        .p99  = pct(0.99),
        .p999 = pct(0.999),
        .max  = sorted.back(),
    };
}

// rdtsc_ns_per_tick — one-time calibration: nanoseconds per TSC tick.
// Runs a ~10 ms spin measured against steady_clock, returns ns/tick.
// Called once from MatchingEngine constructor (untimed region).
double rdtsc_ns_per_tick() noexcept
{
    using namespace std::chrono;
    // Warmup: let the CPU reach steady frequency before measuring.
    {
        volatile std::uint64_t s = 0;
        const std::uint64_t r0 = rdtsc();
        for (std::uint64_t i = 0; i < 1'000'000; ++i) s += i;
        (void)(rdtsc() - r0); (void)s;
    }
    // Timed pass: spin ~10 ms and compare TSC ticks to wall ns.
    const auto    wt0 = steady_clock::now();
    const std::uint64_t rt0 = rdtsc();
    {
        volatile std::uint64_t s = 0;
        for (std::uint64_t i = 0; i < 20'000'000; ++i) s += i;
        (void)s;
    }
    const std::uint64_t rt1 = rdtsc();
    const auto    wt1 = steady_clock::now();
    const double wall_ns = static_cast<double>(
        duration_cast<nanoseconds>(wt1 - wt0).count());
    const double ticks   = static_cast<double>(rt1 - rt0);
    return (ticks > 0.0 && wall_ns > 0.0) ? wall_ns / ticks : 0.33;  // fallback 3 GHz
}

// OrderBook constructor
// Phase 4 Change 3: TradeCallback and level_capacity removed.
// Matching logic lives in MatchingEngine — book is pure data.
OrderBook::OrderBook(std::string symbol,
                     std::size_t pool_size,
                     Price       base_tick)
    : symbol_   (std::move(symbol))
    , pool_     (pool_size)
    , index_    (pool_size)
    , bids_     (index_, pool_, base_tick)
    , asks_     (index_, pool_, base_tick)
{}

// Queries
std::optional<Price> OrderBook::best_bid() const
{
    return bids_.empty() ? std::nullopt
                         : std::optional<Price>{bids_.best_price()};
}
std::optional<Price> OrderBook::best_ask() const
{
    return asks_.empty() ? std::nullopt
                         : std::optional<Price>{asks_.best_price()};
}
std::optional<Price> OrderBook::mid_price() const
{
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return (*bid + *ask) / 2;
}
std::optional<Price> OrderBook::spread() const
{
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}
std::vector<std::pair<Price, Quantity>> OrderBook::bid_depth(std::size_t n) const
{
    return bids_.depth(n);
}
std::vector<std::pair<Price, Quantity>> OrderBook::ask_depth(std::size_t n) const
{
    return asks_.depth(n);
}

// Debug print
void OrderBook::print_top(std::size_t levels) const
{
    auto asks = ask_depth(levels);
    auto bids = bid_depth(levels);
    std::cout << "\n=== " << symbol_ << " ===\n";
    std::cout << std::setw(14) << "PRICE"
              << std::setw(14) << "QTY"
              << "\n";
    // Print asks top-down (highest ask first).
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::cout << "  ASK"
                  << std::setw(14) << it->first
                  << std::setw(14) << it->second
                  << "\n";
    if (auto sp = spread())
        std::cout << "   spread " << *sp << " ticks\n";
    for (auto& [p, q] : bids)
        std::cout << "  BID"
                  << std::setw(14) << p
                  << std::setw(14) << q
                  << "\n";
    auto perc = stats_.compute();
    std::cout << "\nLatency (ns) | orders=" << order_count()
              << " trades=" << trade_count() << "\n"
              << "  min="  << perc.min
              << " p50="   << perc.p50
              << " p99="   << perc.p99
              << " p999="  << perc.p999
              << " max="   << perc.max << "\n\n";
}

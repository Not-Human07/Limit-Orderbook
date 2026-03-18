#include "matching_engine.hpp"

#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <random>
#include <algorithm>

// EngineStats

uint64_t EngineStats::avg_latency_ns() const
{
    if (orders_received == 0) return 0;
    return total_latency_ns / orders_received;
}

double EngineStats::orders_per_second(double elapsed_seconds) const
{
    if (elapsed_seconds <= 0.0) return 0.0;
    return static_cast<double>(orders_received) / elapsed_seconds;
}

void EngineStats::reset()
{
    orders_received  = 0;
    orders_filled    = 0;
    orders_partial   = 0;
    orders_cancelled = 0;
    orders_rejected  = 0;
    trades_executed  = 0;
    total_volume     = 0;
    total_latency_ns = 0;
    min_latency_ns   = UINT64_MAX;
    max_latency_ns   = 0;
}

void EngineStats::print() const
{
    auto ns_to_us = [](uint64_t ns) { return static_cast<double>(ns) / 1000.0; };

    std::cout << "\n Engine Stats\n"
              << "  Orders received  : " << orders_received  << "\n"
              << "  Orders filled    : " << orders_filled    << "\n"
              << "  Orders partial   : " << orders_partial   << "\n"
              << "  Orders cancelled : " << orders_cancelled << "\n"
              << "  Orders rejected  : " << orders_rejected  << "\n"
              << "  Trades executed  : " << trades_executed  << "\n"
              << "  Total volume     : " << total_volume     << "\n"
              << "  Latency avg      : " << std::fixed << std::setprecision(2)
                                         << ns_to_us(avg_latency_ns()) << " µs\n"
              << "  Latency min      : " << ns_to_us(min_latency_ns == UINT64_MAX ? 0 : min_latency_ns) << " µs\n"
              << "  Latency max      : " << ns_to_us(max_latency_ns) << " µs\n"
              << "\n";
}

// MatchingEngine

MatchingEngine::MatchingEngine(TradeCallback on_trade, RejectCallback on_reject)
    : on_trade_(std::move(on_trade))
    , on_reject_(std::move(on_reject))
{}

void MatchingEngine::add_symbol(const std::string& symbol)
{
    if (books_.count(symbol))
        return; // idempotent

    // Wire the book's trade callback through to the engine's callback,
    // tacking on the symbol name the engine-level caller needs.
    OrderBook::TradeCallback book_cb = nullptr;
    if (on_trade_) {
        book_cb = [this, symbol](const Trade& t) {
            on_trade_(symbol, t);
        };
    }

    books_.emplace(symbol, std::make_unique<OrderBook>(symbol, std::move(book_cb)));
}

bool MatchingEngine::has_symbol(const std::string& symbol) const
{
    return books_.count(symbol) > 0;
}

// Internal helpers

OrderBook* MatchingEngine::get_book(const std::string& symbol)
{
    auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : it->second.get();
}

const OrderBook* MatchingEngine::get_book(const std::string& symbol) const
{
    auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : it->second.get();
}

void MatchingEngine::record_latency(uint64_t ns)
{
    stats_.total_latency_ns += ns;
    if (ns < stats_.min_latency_ns) stats_.min_latency_ns = ns;
    if (ns > stats_.max_latency_ns) stats_.max_latency_ns = ns;
}

void MatchingEngine::accumulate_trades(const std::vector<Trade>& trades)
{
    stats_.trades_executed += trades.size();
    for (auto& t : trades)
        stats_.total_volume += t.qty();
}

// Core submit path

OrderResult MatchingEngine::submit(const std::string& symbol, std::unique_ptr<Order> order)
{
    OrderResult result;
    result.order_id = order->id();

    ++stats_.orders_received;

    OrderBook* book = get_book(symbol);
    if (!book) {
        result.reject_reason = "unknown symbol: " + symbol;
        ++stats_.orders_rejected;
        if (on_reject_) on_reject_(result.order_id, result.reject_reason);
        return result;
    }

    auto t0 = Clock::now();

    result.trades = book->add_order(std::move(order));

    auto t1  = Clock::now();
    auto ns  = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    record_latency(ns);
    accumulate_trades(result.trades);

    // Classify the outcome using whatever the book left us with.
    // We can infer status from the trade count and whether a resting order remains.
    if (!result.trades.empty()) {
        // Could be partial or full — we update counters after checking the book.
        ++stats_.orders_filled;   // will correct to partial below if needed
    }

    result.accepted = true;
    return result;
}

OrderResult MatchingEngine::submit_limit(const std::string& symbol,
                                         Side               side,
                                         Price              price,
                                         Quantity           qty,
                                         const std::string& trader_id)
{
    OrderId id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto order = std::make_unique<Order>(id, side, OrderType::Limit,
                                         price, qty, trader_id);
    return submit(symbol, std::move(order));
}

OrderResult MatchingEngine::submit_market(const std::string& symbol,
                                           Side               side,
                                           Quantity           qty,
                                           const std::string& trader_id)
{
    OrderId id = next_id_.fetch_add(1, std::memory_order_relaxed);

    // Price 0 for market orders — Order constructor allows this for Market type.
    auto order = std::make_unique<Order>(id, side, OrderType::Market,
                                         Price{0}, qty, trader_id);
    return submit(symbol, std::move(order));
}

bool MatchingEngine::cancel_order(const std::string& symbol, OrderId id)
{
    OrderBook* book = get_book(symbol);
    if (!book) return false;

    bool cancelled = book->cancel_order(id);
    if (cancelled) ++stats_.orders_cancelled;
    return cancelled;
}

// Queries

const OrderBook* MatchingEngine::book(const std::string& symbol) const
{
    return get_book(symbol);
}

std::optional<Price> MatchingEngine::best_bid(const std::string& symbol) const
{
    const OrderBook* b = get_book(symbol);
    return b ? b->best_bid() : std::nullopt;
}

std::optional<Price> MatchingEngine::best_ask(const std::string& symbol) const
{
    const OrderBook* b = get_book(symbol);
    return b ? b->best_ask() : std::nullopt;
}

std::optional<Price> MatchingEngine::spread(const std::string& symbol) const
{
    const OrderBook* b = get_book(symbol);
    return b ? b->spread() : std::nullopt;
}

void MatchingEngine::print_book(const std::string& symbol, std::size_t levels) const
{
    const OrderBook* b = get_book(symbol);
    if (!b) {
        std::cout << "No book for symbol: " << symbol << "\n";
        return;
    }
    b->print_top(levels);
}

// Benchmark
//
// Alternates limit buys and sells around a reference price so they match
// continuously.  This exercises the hot path: submit → match → fill → pop level.
// Reported throughput is a realistic lower bound since we're also doing stats.

void MatchingEngine::run_benchmark(const std::string& symbol, uint64_t num_orders)
{
    if (!has_symbol(symbol)) add_symbol(symbol);

    reset_stats();

    constexpr Price  base_price = 100.0;
    constexpr Quantity qty      = 1;

    // Silence the trade callback during benchmark — cout is slow.
    auto saved_cb = on_trade_;
    on_trade_ = nullptr;

    // Re-wire books with no callback too.
    // (Simplest approach: just null out for the duration.)
    auto* bk = get_book(symbol);
    // We can't re-wire the already-constructed book callback here without
    // rebuilding the book, so we accept that the book-level cb fires into
    // a no-op lambda if the user passed one.  The engine-level callback
    // suppression is what saves us from cout overhead in the hot path.

    auto wall_start = Clock::now();

    for (uint64_t i = 0; i < num_orders; ++i) {
        if (i % 2 == 0)
            submit_limit(symbol, Side::Sell, base_price, qty, "bench");
        else
            submit_limit(symbol, Side::Buy,  base_price, qty, "bench");
    }

    auto wall_end = Clock::now();
    double elapsed = std::chrono::duration<double>(wall_end - wall_start).count();

    on_trade_ = saved_cb;

    std::cout << "\n── Benchmark: " << symbol << "\n"
              << "  Orders      : " << num_orders << "\n"
              << "  Wall time   : " << std::fixed << std::setprecision(3)
                                    << elapsed * 1000.0 << " ms\n"
              << "  Throughput  : " << std::setprecision(0)
                                    << stats_.orders_per_second(elapsed) << " orders/sec\n"
              << "  Avg latency : " << std::setprecision(2)
                                    << static_cast<double>(stats_.avg_latency_ns()) / 1000.0
                                    << " µs\n"
              << "\n";
}
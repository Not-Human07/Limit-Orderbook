#include "matching_engine.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>


// EngineStats
std::uint64_t EngineStats::avg_latency_ns() const noexcept
{
    // Guard against division by zero on an empty run.
    return (orders_received == 0) ? 0 : (total_latency_ns / orders_received);
}

double EngineStats::orders_per_second(double elapsed_seconds) const noexcept
{
    if (elapsed_seconds <= 0.0) return 0.0;
    return static_cast<double>(orders_received) / elapsed_seconds;
}

void EngineStats::reset() noexcept
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
    // Called offline only — formatting cost is irrelevant.
    const std::uint64_t avg = avg_latency_ns();

    std::cout << "\n=== EngineStats ===\n"
              << std::setw(24) << "orders_received"  << " : " << orders_received  << "\n"
              << std::setw(24) << "orders_filled"    << " : " << orders_filled    << "\n"
              << std::setw(24) << "orders_partial"   << " : " << orders_partial   << "\n"
              << std::setw(24) << "orders_cancelled" << " : " << orders_cancelled << "\n"
              << std::setw(24) << "orders_rejected"  << " : " << orders_rejected  << "\n"
              << std::setw(24) << "trades_executed"  << " : " << trades_executed  << "\n"
              << std::setw(24) << "total_volume"     << " : " << total_volume     << "\n"
              << "\nLatency (ns):\n"
              << std::setw(24) << "  avg"            << " : " << avg              << "\n"
              << std::setw(24) << "  min"            << " : " << min_latency_ns   << "\n"
              << std::setw(24) << "  max"            << " : " << max_latency_ns   << "\n\n";
}



// MatchingEngine — construction

MatchingEngine::MatchingEngine(EngineTradeCallback  on_trade,
                               EngineRejectCallback on_reject)
    : on_trade_ (std::move(on_trade))
    , on_reject_(std::move(on_reject))
{}



// Symbol registration

void MatchingEngine::add_symbol(const std::string& symbol)
{
    if (symbol.empty())
        throw std::invalid_argument("symbol must be non-empty");

    // Silently ignore duplicate registrations — idempotent.
    if (books_.count(symbol)) return;

    // Each OrderBook owns its own pool and index.
    // The per-book TradeCallback wraps the engine-level callback, prepending
    // the symbol.  Capturing by value is intentional: the lambda must outlive
    // any individual add_symbol call.
    TradeCallback book_cb = nullptr;
    if (on_trade_) {
        book_cb = [this, symbol](const Trade& t) {
            on_trade_(symbol, t);
        };
    }

    books_.emplace(symbol,
                   std::make_unique<OrderBook>(symbol, std::move(book_cb)));
}

bool MatchingEngine::has_symbol(const std::string& symbol) const
{
    return books_.count(symbol) != 0;
}


// ============================================================================
// Internal helpers
// ============================================================================

OrderBook* MatchingEngine::get_book(const std::string& symbol)
{
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : it->second.get();
}

const OrderBook* MatchingEngine::get_book(const std::string& symbol) const
{
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : it->second.get();
}

void MatchingEngine::record_latency(std::uint64_t ns) noexcept
{
    stats_.total_latency_ns += ns;
    if (ns < stats_.min_latency_ns) stats_.min_latency_ns = ns;
    if (ns > stats_.max_latency_ns) stats_.max_latency_ns = ns;
}

void MatchingEngine::accumulate_trades(const std::vector<Trade>& trades) noexcept
{
    stats_.trades_executed += trades.size();
    for (const Trade& t : trades)
        stats_.total_volume += t.quantity;
}



// do_submit — single internal path for all order types
//
// All public submit_* methods funnel here.  This keeps stat accounting,
// latency measurement, and error handling in exactly one place.
//
// Design notes:
//   - OrderId is assigned here, monotonically, before the book sees it.
//   - Timing wraps add_order only — symbol lookup is outside the timed region
//     so the number reflects pure engine latency, not map lookup overhead.
//   - OrderResult::accepted is derived from the trade vector and order type:
//       * FOK rejection  → add_order returns empty trades  (book never touched)
//       * Market/IOC     → accepted=true regardless of fill amount
//       * GTC partial    → accepted=true, trades may be non-empty


OrderResult MatchingEngine::do_submit(const std::string& symbol,
                                      Side               side,
                                      Price              price,
                                      Quantity           qty,
                                      OrderType          type,
                                      TIF                tif)
{
    OrderBook* bk = get_book(symbol);
    if (!bk) [[unlikely]] {
        // Unknown symbol — reject immediately, no id assigned.
        OrderResult r;
        r.order_id     = 0;
        r.accepted     = false;
        r.reject_reason = "unknown symbol: " + symbol;
        ++stats_.orders_rejected;
        if (on_reject_) on_reject_(0, r.reject_reason);
        return r;
    }

    const OrderId id = next_id_++;
    ++stats_.orders_received;

    // timed region starts
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<Trade> trades = bk->add_order(id, price, qty, side, type, tif);

    const auto t1 = std::chrono::steady_clock::now();
    // timed region ends 

    const std::uint64_t elapsed_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    record_latency(elapsed_ns);
    accumulate_trades(trades);

    // Determine acceptance.
    // FOK rejection: tif==FOK && trades empty && order is NOT in the book
    // (add_order returns {} on FOK reject and the order never rests).
    // We distinguish FOK rejection from a GTC/IOC that simply had no matches
    // by checking the tif — an empty trade vector on a non-FOK is still accepted.
    const bool fok_rejected = (tif == TIF::FOK) && trades.empty();

    OrderResult result;
    result.order_id = id;
    result.trades   = std::move(trades);

    if (fok_rejected) {
        result.accepted      = false;
        result.reject_reason = "FOK: insufficient liquidity";
        ++stats_.orders_rejected;
        if (on_reject_) on_reject_(id, result.reject_reason);
    } else {
        result.accepted = true;

        // Update fill/partial/resting counters.
        // We infer final order status from the trade list + tif:
        //   - If all qty was filled in trades → Filled
        //   - If some was filled and IOC/Market → also counts as done (cancelled remainder)
        //   - If some was filled and GTC → PartiallyFilled, resting
        //   - If no fills and GTC → New, resting (not counted as partial)
        Quantity filled = 0;
        for (const Trade& t : result.trades)
            filled += t.quantity;

        if (filled == qty) {
            ++stats_.orders_filled;
        } else if (filled > 0) {
            // Partial fill.
            ++stats_.orders_partial;
        }
        // Zero-fill GTC limit that rests fully → no counter update (it's just resting).
    }

    return result;
}



// Public submission API


OrderResult MatchingEngine::submit_limit(const std::string& symbol,
                                         Side               side,
                                         Price              price,
                                         Quantity           qty,
                                         TIF                tif)
{
    if (qty == 0) [[unlikely]]
        throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]]
        throw std::invalid_argument("limit price must be > 0");

    return do_submit(symbol, side, price, qty, OrderType::Limit, tif);
}

OrderResult MatchingEngine::submit_market(const std::string& symbol,
                                          Side               side,
                                          Quantity           qty)
{
    if (qty == 0) [[unlikely]]
        throw std::invalid_argument("order quantity must be > 0");

    // Market orders use MARKET_PRICE sentinel and IOC semantics:
    // sweep as much liquidity as exists, discard any unfilled remainder.
    return do_submit(symbol, side, MARKET_PRICE, qty, OrderType::Market, TIF::IOC);
}

OrderResult MatchingEngine::submit_fok(const std::string& symbol,
                                       Side               side,
                                       Price              price,
                                       Quantity           qty)
{
    if (qty == 0) [[unlikely]]
        throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]]
        throw std::invalid_argument("limit price must be > 0");

    return do_submit(symbol, side, price, qty, OrderType::Limit, TIF::FOK);
}

OrderResult MatchingEngine::submit_ioc(const std::string& symbol,
                                       Side               side,
                                       Price              price,
                                       Quantity           qty)
{
    if (qty == 0) [[unlikely]]
        throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]]
        throw std::invalid_argument("limit price must be > 0");

    return do_submit(symbol, side, price, qty, OrderType::Limit, TIF::IOC);
}

bool MatchingEngine::cancel_order(const std::string& symbol, OrderId id)
{
    OrderBook* bk = get_book(symbol);
    if (!bk) return false;

    const bool cancelled = bk->cancel_order(id);
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
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->best_bid() : std::nullopt;
}

std::optional<Price> MatchingEngine::best_ask(const std::string& symbol) const
{
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->best_ask() : std::nullopt;
}

std::optional<Price> MatchingEngine::spread(const std::string& symbol) const
{
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->spread() : std::nullopt;
}

std::optional<Price> MatchingEngine::mid_price(const std::string& symbol) const
{
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->mid_price() : std::nullopt;
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::bid_depth(const std::string& symbol, std::size_t levels) const
{
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->bid_depth(levels) : std::vector<std::pair<Price, Quantity>>{};
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::ask_depth(const std::string& symbol, std::size_t levels) const
{
    const OrderBook* bk = get_book(symbol);
    return bk ? bk->ask_depth(levels) : std::vector<std::pair<Price, Quantity>>{};
}

void MatchingEngine::print_book(const std::string& symbol, std::size_t levels) const
{
    const OrderBook* bk = get_book(symbol);
    if (!bk) {
        std::cout << "[MatchingEngine] unknown symbol: " << symbol << "\n";
        return;
    }
    bk->print_top(levels);
}

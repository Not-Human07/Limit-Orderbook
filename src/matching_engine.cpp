#include "matching_engine.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>


// EngineStats

std::uint64_t EngineStats::avg_latency_ns() const noexcept
{
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

SymbolId MatchingEngine::add_symbol(const std::string& symbol)
{
    if (symbol.empty())
        throw std::invalid_argument("symbol must be non-empty");

    // Idempotent — return existing id on duplicate registration.
    auto it = symbol_ids_.find(symbol);
    if (it != symbol_ids_.end()) return it->second;

    if (next_symbol_id_ >= MAX_SYMBOLS)
        throw std::runtime_error("symbol table full (max 256 symbols)");

    const SymbolId sid = next_symbol_id_++;

    // Per-book callback wraps the engine-level callback with the symbol name.
    // Captured by value — lambda must outlive the add_symbol call.
    TradeCallback book_cb = nullptr;
    if (on_trade_) {
        book_cb = [this, symbol](const Trade& t) {
            on_trade_(symbol, t);
        };
    }

    books_[sid] = std::make_unique<OrderBook>(symbol, std::move(book_cb));
    symbol_ids_[symbol] = sid;
    return sid;
}

bool MatchingEngine::has_symbol(const std::string& symbol) const
{
    return symbol_ids_.count(symbol) != 0;
}



// Internal helpers

// O(1): single bounds check + array slot read.  No string work.
OrderBook* MatchingEngine::get_book(SymbolId sid) noexcept
{
    return (sid < MAX_SYMBOLS) ? books_[sid].get() : nullptr;
}

const OrderBook* MatchingEngine::get_book(SymbolId sid) const noexcept
{
    return (sid < MAX_SYMBOLS) ? books_[sid].get() : nullptr;
}

// Returns MAX_SYMBOLS as sentinel when symbol is unknown.
// get_book(MAX_SYMBOLS) returns nullptr, which do_submit handles as rejection.
// Never called on the hot path.
SymbolId MatchingEngine::resolve(const std::string& symbol) const noexcept
{
    auto it = symbol_ids_.find(symbol);
    return (it == symbol_ids_.end()) ? MAX_SYMBOLS : it->second;
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
// All public submit_* methods funnel here.  Stat accounting, latency
// measurement, and error handling live in exactly one place.
//
// Takes SymbolId — all symbol resolution is done by callers.
// The string path (resolve → SymbolId → do_submit) is off the hot path.

OrderResult MatchingEngine::do_submit(SymbolId  sid,
                                      Side      side,
                                      Price     price,
                                      Quantity  qty,
                                      OrderType type,
                                      TIF       tif)
{
    // Single array slot read — no string hash, no bucket scan.
    OrderBook* bk = get_book(sid);
    if (!bk) [[unlikely]] {
        OrderResult r;
        r.order_id      = 0;
        r.accepted      = false;
        r.reject_reason = "unknown symbol";
        ++stats_.orders_rejected;
        if (on_reject_) on_reject_(0, r.reject_reason);
        return r;
    }

    const OrderId id = next_id_++;
    ++stats_.orders_received;

    // add_order times itself internally; last_ns() reads that sample back.
    // Avoids two redundant Clock::now() calls that would bracket add_order here.
    std::vector<Trade> trades = bk->add_order(id, price, qty, side, type, tif);
    const std::uint64_t elapsed_ns = bk->latency_stats().last_ns();

    record_latency(elapsed_ns);
    accumulate_trades(trades);

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

        Quantity filled = 0;
        for (const Trade& t : result.trades)
            filled += t.quantity;

        if (filled == qty) {
            ++stats_.orders_filled;
        } else if (filled > 0) {
            ++stats_.orders_partial;
        }
    }

    return result;
}



// Public submission API — SymbolId overloads (hot path)
// Single array slot read in do_submit; no string work anywhere.


OrderResult MatchingEngine::submit_limit(SymbolId sid, Side side, Price price,
                                          Quantity qty, TIF tif)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(sid, side, price, qty, OrderType::Limit, tif);
}

OrderResult MatchingEngine::submit_market(SymbolId sid, Side side, Quantity qty)
{
    if (qty == 0) [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    return do_submit(sid, side, MARKET_PRICE, qty, OrderType::Market, TIF::IOC);
}

OrderResult MatchingEngine::submit_fok(SymbolId sid, Side side, Price price,
                                        Quantity qty)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(sid, side, price, qty, OrderType::Limit, TIF::FOK);
}

OrderResult MatchingEngine::submit_ioc(SymbolId sid, Side side, Price price,
                                        Quantity qty)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(sid, side, price, qty, OrderType::Limit, TIF::IOC);
}

bool MatchingEngine::cancel_order(SymbolId sid, OrderId id)
{
    OrderBook* bk = get_book(sid);
    if (!bk) return false;
    const bool cancelled = bk->cancel_order(id);
    if (cancelled) ++stats_.orders_cancelled;
    return cancelled;
}



// Public submission API — string wrappers (off hot path)
// Resolve symbol → SymbolId once, then delegate to do_submit.


OrderResult MatchingEngine::submit_limit(const std::string& symbol, Side side,
                                          Price price, Quantity qty, TIF tif)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(resolve(symbol), side, price, qty, OrderType::Limit, tif);
}

OrderResult MatchingEngine::submit_market(const std::string& symbol, Side side,
                                           Quantity qty)
{
    if (qty == 0) [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    return do_submit(resolve(symbol), side, MARKET_PRICE, qty, OrderType::Market,
                     TIF::IOC);
}

OrderResult MatchingEngine::submit_fok(const std::string& symbol, Side side,
                                        Price price, Quantity qty)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(resolve(symbol), side, price, qty, OrderType::Limit, TIF::FOK);
}

OrderResult MatchingEngine::submit_ioc(const std::string& symbol, Side side,
                                        Price price, Quantity qty)
{
    if (qty == 0)   [[unlikely]] throw std::invalid_argument("order quantity must be > 0");
    if (price <= 0) [[unlikely]] throw std::invalid_argument("limit price must be > 0");
    return do_submit(resolve(symbol), side, price, qty, OrderType::Limit, TIF::IOC);
}

bool MatchingEngine::cancel_order(const std::string& symbol, OrderId id)
{
    return cancel_order(resolve(symbol), id);
}



// Queries — SymbolId overloads


const OrderBook* MatchingEngine::book(SymbolId sid) const
{
    return get_book(sid);
}

std::optional<Price> MatchingEngine::best_bid(SymbolId sid) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->best_bid() : std::nullopt;
}

std::optional<Price> MatchingEngine::best_ask(SymbolId sid) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->best_ask() : std::nullopt;
}

std::optional<Price> MatchingEngine::spread(SymbolId sid) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->spread() : std::nullopt;
}

std::optional<Price> MatchingEngine::mid_price(SymbolId sid) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->mid_price() : std::nullopt;
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::bid_depth(SymbolId sid, std::size_t levels) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->bid_depth(levels) : std::vector<std::pair<Price, Quantity>>{};
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::ask_depth(SymbolId sid, std::size_t levels) const
{
    const OrderBook* bk = get_book(sid);
    return bk ? bk->ask_depth(levels) : std::vector<std::pair<Price, Quantity>>{};
}

void MatchingEngine::print_book(SymbolId sid, std::size_t levels) const
{
    const OrderBook* bk = get_book(sid);
    if (!bk) {
        std::cout << "[MatchingEngine] unknown symbol id\n";
        return;
    }
    bk->print_top(levels);
}



// Queries — string wrappers


const OrderBook* MatchingEngine::book(const std::string& symbol) const
{
    return book(resolve(symbol));
}

std::optional<Price> MatchingEngine::best_bid(const std::string& symbol) const
{
    return best_bid(resolve(symbol));
}

std::optional<Price> MatchingEngine::best_ask(const std::string& symbol) const
{
    return best_ask(resolve(symbol));
}

std::optional<Price> MatchingEngine::spread(const std::string& symbol) const
{
    return spread(resolve(symbol));
}

std::optional<Price> MatchingEngine::mid_price(const std::string& symbol) const
{
    return mid_price(resolve(symbol));
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::bid_depth(const std::string& symbol, std::size_t levels) const
{
    return bid_depth(resolve(symbol), levels);
}

std::vector<std::pair<Price, Quantity>>
MatchingEngine::ask_depth(const std::string& symbol, std::size_t levels) const
{
    return ask_depth(resolve(symbol), levels);
}

void MatchingEngine::print_book(const std::string& symbol, std::size_t levels) const
{
    print_book(resolve(symbol), levels);
}

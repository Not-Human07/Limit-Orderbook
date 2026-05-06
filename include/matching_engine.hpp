#pragma once

// Top-level orchestrator.  Owns a map of symbols → OrderBooks and exposes
// a clean submission / cancellation API.

// Thread safety: NOT thread-safe by design.
// For multi-symbol parallelism, shard by symbol — one engine instance per shard.

#include "order_book.hpp"   // transitively pulls in order.hpp

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


// EngineStats
// Accumulated counters since engine start (or last reset()).
// Plain values — single-threaded contract, no atomics needed.

struct EngineStats {
    std::uint64_t orders_received  { 0 };
    std::uint64_t orders_filled    { 0 };   //< Fully filled.
    std::uint64_t orders_partial   { 0 };   //< Resting with at least one fill.
    std::uint64_t orders_cancelled { 0 };
    std::uint64_t orders_rejected  { 0 };   //< FOK rejections.
    std::uint64_t trades_executed  { 0 };
    Quantity       total_volume    { 0 };   //< Total quantity traded (both sides).

    // Latency in nanoseconds (wall-clock, per add_order call).
    std::uint64_t total_latency_ns { 0 };
    std::uint64_t min_latency_ns   { UINT64_MAX };
    std::uint64_t max_latency_ns   { 0 };

    // Derived — computed on demand, not maintained incrementally.
    [[nodiscard]] std::uint64_t avg_latency_ns() const noexcept;
    [[nodiscard]] double        orders_per_second(double elapsed_seconds) const noexcept;

    void reset()  noexcept;
    void print()  const;
};


// OrderResult

// Returned by every submit_* call.
// "accepted" is false only for FOK orders that could not fill fully.
// "trades"   may be non-empty even when accepted == true and the order
//  partially rests (IOC / GTC with an immediate partial fill).

struct OrderResult {
    OrderId            order_id    { 0 };
    bool               accepted    { false };
    std::string        reject_reason;          //< Non-empty iff accepted == false.
    std::vector<Trade> trades;                 //< Immediate fills, if any.
};


// EngineTradeCallback
// Distinct from the per-book TradeCallback (order.hpp) which carries only
// the Trade.  The engine-level callback adds the symbol so upstream consumers
// can route without a separate lookup.
//
// Named differently to avoid any collision with the TradeCallback alias
// already defined in order.hpp.

using EngineTradeCallback  = std::function<void(const std::string& symbol,
                                                 const Trade&)>;
using EngineRejectCallback = std::function<void(OrderId,
                                                const std::string& reason)>;


// MatchingEngine

class MatchingEngine {
public:
    explicit MatchingEngine(EngineTradeCallback  on_trade  = nullptr,
                            EngineRejectCallback on_reject = nullptr);
    // Symbol registration
    // Must be called before any orders for that symbol are submitted.
      void add_symbol(const std::string& symbol);
    [[nodiscard]] bool has_symbol(const std::string& symbol) const;

    // Order submission
    // Submit a GTC limit order.
    // Returns an OrderResult with the generated id and any immediate fills.
    OrderResult submit_limit(const std::string& symbol,
                             Side               side,
                             Price              price,
                             Quantity           qty,
                             TIF                tif = TIF::GTC);

    /// Submit a market order (sweeps the book; never rests).
    /// tif is always IOC semantics for market orders — remainder is dropped.
    OrderResult submit_market(const std::string& symbol,
                              Side               side,
                              Quantity           qty);

    /// Submit a FOK limit order.
    /// Convenience wrapper — equivalent to submit_limit with tif=TIF::FOK.
    OrderResult submit_fok(const std::string& symbol,
                           Side               side,
                           Price              price,
                           Quantity           qty);

    /// Submit an IOC limit order.
    /// Convenience wrapper — equivalent to submit_limit with tif=TIF::IOC.
    OrderResult submit_ioc(const std::string& symbol,
                           Side               side,
                           Price              price,
                           Quantity           qty);

    /// Cancel a resting order by id.
    /// Returns false if not found or already terminal (filled / cancelled).
    bool cancel_order(const std::string& symbol, OrderId id);

    
    // Queries (non-mutating)
   

    /// Raw access to the underlying OrderBook for a symbol.
    /// Returns nullptr if the symbol is not registered.
    [[nodiscard]] const OrderBook* book(const std::string& symbol) const;

    [[nodiscard]] std::optional<Price> best_bid(const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> best_ask(const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> spread   (const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> mid_price(const std::string& symbol) const;

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>>
    bid_depth(const std::string& symbol, std::size_t levels = 10) const;

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>>
    ask_depth(const std::string& symbol, std::size_t levels = 10) const;

    // Stats & diagnostics

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }
    void print_stats() const    { stats_.print(); }

    // Delegates to OrderBook::print_top for the given symbol.
    void print_book(const std::string& symbol, std::size_t levels = 5) const;

private:
   // Internal helpers
   
    //Core dispatch, both submit_limit and submit_market funnel here.
    /// "price"    : pass MARKET_PRICE for market orders.
    /// "type"    : Limit or Market.
    /// "tif"      : GTC / IOC / FOK.
    OrderResult do_submit(const std::string& symbol,
                          Side               side,
                          Price              price,
                          Quantity           qty,
                          OrderType          type,
                          TIF                tif);

    // Returns a mutable pointer to the book, or nullptr.
    OrderBook*       get_book(const std::string& symbol);
    const OrderBook* get_book(const std::string& symbol) const;

    // Update latency stats after a single add_order call.
    void record_latency(std::uint64_t ns) noexcept;

    // Walk the trade vector and update stats_.
    void accumulate_trades(const std::vector<Trade>& trades) noexcept;

    // Members
    // Monotonically increasing order id — plain integer, single-threaded.
    OrderId next_id_ { 1 };

    // One OrderBook per registered symbol.
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;

    EngineTradeCallback  on_trade_;
    EngineRejectCallback on_reject_;

    EngineStats stats_;
};

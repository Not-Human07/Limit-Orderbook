#pragma once

// Top-level orchestrator.  Owns a map of symbols → OrderBooks and exposes
// a clean submission / cancellation API.

// Thread safety: NOT thread-safe by design.
// For multi-symbol parallelism, shard by symbol — one engine instance per shard.

#include "order_book.hpp"   // transitively pulls in order.hpp

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


// SymbolId
// Lightweight integer handle returned by add_symbol().
// Store at setup time and pass to the SymbolId overloads of submit_*,
// cancel_order, and queries.  Replaces the string hash + bucket lookup
// that fires on every order in the string overloads.

using SymbolId = std::uint16_t;


// EngineStats
// Accumulated counters since engine start (or last reset()).
// Plain values — single-threaded contract, no atomics needed.

struct EngineStats {
    std::uint64_t orders_received  { 0 };
    std::uint64_t orders_filled    { 0 };   ///< Fully filled.
    std::uint64_t orders_partial   { 0 };   ///< Resting with at least one fill.
    std::uint64_t orders_cancelled { 0 };
    std::uint64_t orders_rejected  { 0 };   ///< FOK rejections.
    std::uint64_t trades_executed  { 0 };
    Quantity       total_volume    { 0 };   ///< Total quantity traded (both sides).

    // Latency in nanoseconds (wall-clock, per add_order call).
    std::uint64_t total_latency_ns { 0 };
    std::uint64_t min_latency_ns   { UINT64_MAX };
    std::uint64_t max_latency_ns   { 0 };

    // Derived — computed on demand, not maintained incrementally.
    [[nodiscard]] std::uint64_t avg_latency_ns() const noexcept;
    [[nodiscard]] double        orders_per_second(double elapsed_seconds) const noexcept;

    void reset() noexcept;
    void print() const;
};


// OrderResult

// Returned by every submit_* call.
// "accepted" is false only for FOK orders that could not fill fully.
// "trades"   may be non-empty even when accepted == true and the order
//  partially rests (IOC / GTC with an immediate partial fill).

struct OrderResult {
    OrderId            order_id    { 0 };
    bool               accepted    { false };
    std::string        reject_reason;          ///< Non-empty iff accepted == false.
    std::vector<Trade> trades;                 ///< Immediate fills, if any.
};


// EngineTradeCallback
// Distinct from the per-book TradeCallback (order.hpp) which carries only
// the Trade.  The engine-level callback adds the symbol so upstream consumers
// can route without a separate lookup.

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
    // Returns a SymbolId — store it and pass it to the SymbolId overloads
    // of submit_*, cancel_order, and queries to eliminate string work on
    // every hot-path call.
    SymbolId add_symbol(const std::string& symbol);
    [[nodiscard]] bool has_symbol(const std::string& symbol) const;

    // Hot-path overloads (SymbolId)
    // Single array slot read — no string hash, no bucket scan.

    OrderResult submit_limit (SymbolId sid, Side side, Price price, Quantity qty,
                              TIF tif = TIF::GTC);
    OrderResult submit_market(SymbolId sid, Side side, Quantity qty);
    OrderResult submit_fok   (SymbolId sid, Side side, Price price, Quantity qty);
    OrderResult submit_ioc   (SymbolId sid, Side side, Price price, Quantity qty);
    bool        cancel_order (SymbolId sid, OrderId id);

    // String wrappers
    // Resolve symbol → SymbolId once, then delegate.  Never on the hot path.

    OrderResult submit_limit (const std::string& symbol, Side side, Price price,
                              Quantity qty, TIF tif = TIF::GTC);
    OrderResult submit_market(const std::string& symbol, Side side, Quantity qty);
    OrderResult submit_fok   (const std::string& symbol, Side side, Price price,
                              Quantity qty);
    OrderResult submit_ioc   (const std::string& symbol, Side side, Price price,
                              Quantity qty);
    bool        cancel_order (const std::string& symbol, OrderId id);

    // Queries (SymbolId)
    [[nodiscard]] const OrderBook* book(SymbolId sid) const;
    [[nodiscard]] std::optional<Price> best_bid (SymbolId sid) const;
    [[nodiscard]] std::optional<Price> best_ask (SymbolId sid) const;
    [[nodiscard]] std::optional<Price> spread   (SymbolId sid) const;
    [[nodiscard]] std::optional<Price> mid_price(SymbolId sid) const;
    [[nodiscard]] std::vector<std::pair<Price, Quantity>>
        bid_depth(SymbolId sid, std::size_t levels = 10) const;
    [[nodiscard]] std::vector<std::pair<Price, Quantity>>
        ask_depth(SymbolId sid, std::size_t levels = 10) const;
    void print_book(SymbolId sid, std::size_t levels = 5) const;

    // Queries (string)
    [[nodiscard]] const OrderBook* book(const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> best_bid (const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> best_ask (const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> spread   (const std::string& symbol) const;
    [[nodiscard]] std::optional<Price> mid_price(const std::string& symbol) const;
    [[nodiscard]] std::vector<std::pair<Price, Quantity>>
        bid_depth(const std::string& symbol, std::size_t levels = 10) const;
    [[nodiscard]] std::vector<std::pair<Price, Quantity>>
        ask_depth(const std::string& symbol, std::size_t levels = 10) const;
    void print_book(const std::string& symbol, std::size_t levels = 5) const;

    // Stats & diagnostics

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_.reset(); }
    void print_stats() const    { stats_.print(); }

private:
    // Maximum symbols supported.  256 slots.
    static constexpr SymbolId MAX_SYMBOLS = 256;

    // Per-symbol runtime state.
    // book      : owned OrderBook; nullptr = slot unused.
    // order_seq : monotonically increasing sequence number per book.
    //             Replaces the seq_ that used to live in OrderBook.
    struct SymbolState {
        std::unique_ptr<OrderBook> book;
        SeqNum                     order_seq{0};
    };

    // Helpers

    // Single internal submit path — all public submit_* methods funnel here.
    OrderResult do_submit(SymbolId  sid,
                          Side      side,
                          Price     price,
                          Quantity  qty,
                          OrderType type,
                          TIF       tif);

    // Core matching loop — same TU as do_submit so the compiler can inline.
    // Writes fills into trade_ring_.  Returns void — no alloc.
    void  match   (OrderBook& bk, SymbolState& ss,
                   Order& aggressor, bool simulate_only) noexcept;

    // Build one Trade, increment bk.trade_seq_, fire on_trade_ if set.
    Trade make_exec(OrderBook& bk, Order& buy, Order& sell,
                    Price px, Quantity qty);

    // O(1) book lookup — single bounds check + array slot read.
    OrderBook*       get_book(SymbolId sid)       noexcept;
    const OrderBook* get_book(SymbolId sid) const noexcept;

    // String → SymbolId.  Returns MAX_SYMBOLS (sentinel) if unknown.
    // Never called on the hot path.
    SymbolId resolve(const std::string& symbol) const noexcept;

    void record_latency   (std::uint64_t ns)      noexcept;
    void accumulate_trades(const TradeRing& ring)  noexcept;

    // Members

    OrderId  next_id_        { 1 };
    SymbolId next_symbol_id_ { 0 };

    // Direct-indexed per-symbol state — O(1) lookup, no string hash.
    std::array<SymbolState, MAX_SYMBOLS> symbols_{};

    // String → SymbolId — touched only at add_symbol time, never on hot path.
    std::unordered_map<std::string, SymbolId> symbol_ids_;

    // Single TradeRing reused across every do_submit call.
    // Single-threaded contract — one call active at a time.
    TradeRing trade_ring_;

    EngineTradeCallback  on_trade_;
    EngineRejectCallback on_reject_;

    EngineStats stats_;
};

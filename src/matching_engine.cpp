#include "matching_engine.hpp"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <stdexcept>

// RDTSC calibration constant — nanoseconds per tick.
// Initialised once (magic static) before any benchmark runs.
// multiply-by-reciprocal is ~3 cycles; cheaper than dividing by ticks_per_ns.
static const double g_ns_per_tick = rdtsc_ns_per_tick();


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

    // Phase 4 Change 3: OrderBook no longer takes a TradeCallback.
    // MatchingEngine::make_exec() calls on_trade_ directly — one less
    // indirection through a std::function wrapper per fill.
    symbols_[sid].book = std::make_unique<OrderBook>(symbol);
    symbol_ids_[symbol] = sid;
    return sid;
}

bool MatchingEngine::has_symbol(const std::string& symbol) const
{
    return symbol_ids_.count(symbol) != 0;
}



// Internal helpers

OrderBook* MatchingEngine::get_book(SymbolId sid) noexcept
{
    return (sid < MAX_SYMBOLS) ? symbols_[sid].book.get() : nullptr;
}

const OrderBook* MatchingEngine::get_book(SymbolId sid) const noexcept
{
    return (sid < MAX_SYMBOLS) ? symbols_[sid].book.get() : nullptr;
}

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

void MatchingEngine::accumulate_trades(const TradeRing& ring) noexcept
{
    stats_.trades_executed += ring.size();
    for (const Trade& t : ring)
        stats_.total_volume += t.quantity;
}



// make_exec — build one Trade, stamp ts if callback is set, fire callback.
//
// Moved from OrderBook::make_trade() — now in the same TU as match() and
// do_submit(), giving the compiler full visibility for inlining.
// Writes directly to bk.trade_seq_ via friend access.

Trade MatchingEngine::make_exec(OrderBook& bk, Order& buy, Order& sell,
                                 Price px, Quantity qty)
{
    ++bk.trade_seq_;
    Trade t{
        .buy_order_id  = buy.id,
        .sell_order_id = sell.id,
        .price         = px,
        .quantity      = qty,
        .trade_seq     = bk.trade_seq_,
        // ts stamped only when a callback is registered — Clock::now() costs ~23 ns.
        .ts            = on_trade_ ? Clock::now() : TimePoint{},
    };
    // Engine-level callback called directly: no per-book std::function wrapper,
    // no lambda capture overhead on the benchmarked path (on_trade_ is nullptr
    // in all benchmarks).
    if (on_trade_) on_trade_(bk.symbol_, t);
    return t;
}



// match — core matching loop (price-time priority)
//
// Phase 4 Change 3: moved from order_book.cpp to matching_engine.cpp.
// Being in the same TU as do_submit() allows the compiler to inline the full
// order lifecycle — no cross-TU call overhead between do_submit and match.
//
// Writes fills into engine-owned trade_ring_ (no heap allocation).
// Accesses bk.asks_ / bk.bids_ / bk.index_ / bk.pool_ via friend.
//
// Hot path (real fills):
//   side_book.has_best_         — flag check, no indirection
//   lvl.front() / lvl.unlink() — intrusive list, no alloc
//   erase_level()               — flag flip + advance_best() scan
//
// FOK simulate path (simulate_only == true):
//   BookSide::simulate_fill()  — read-only walk, no book mutations.

void MatchingEngine::match(OrderBook& bk, SymbolState& /*ss*/,
                            Order& aggressor, bool simulate_only) noexcept
{
    const bool is_buy = aggressor.side == Side::Buy;

    auto prices_cross = [&](Price resting_px) noexcept -> bool {
        if (aggressor.type == OrderType::Market) return true;
        return is_buy ? (aggressor.price >= resting_px)
                      : (aggressor.price <= resting_px);
    };

    if (simulate_only) {
        // FOK pre-check — no book mutations, just decrement remaining_qty.
        if (is_buy) bk.asks_.simulate_fill(aggressor, prices_cross);
        else        bk.bids_.simulate_fill(aggressor, prices_cross);
        return;
    }

    // Real match path.
    // Templated lambda: compiler sees the concrete BookSide<IsBid> type and
    // can inline erase_level / advance_best with the correct direction.
    auto run = [&](auto& side_book) noexcept {
        while (aggressor.is_active() && side_book.has_best_) {
            PriceLevel& lvl = side_book.levels_[side_book.best_idx_];
            if (!prices_cross(lvl.price)) break;

            while (aggressor.is_active() && !lvl.empty()) {
                Order* resting = lvl.front();
                assert(resting && resting->is_active());

                const Quantity fill_qty = std::min(aggressor.remaining_qty,
                                                   resting->remaining_qty);
                const Price    fill_px  = resting->price;

                aggressor.fill(fill_qty);
                resting->fill(fill_qty);
                lvl.total_qty -= fill_qty;

                Order& buy_o  = is_buy ? aggressor : *resting;
                Order& sell_o = is_buy ? *resting  : aggressor;
                trade_ring_.push(make_exec(bk, buy_o, sell_o, fill_px, fill_qty));

                if (!resting->is_active()) {
                    bk.index_.erase(resting->id);
                    lvl.unlink(resting);
                    bk.pool_.deallocate(resting);
                }
            }

            if (lvl.empty())
                side_book.erase_level(side_book.best_idx_);
        }
    };

    if (is_buy) run(bk.asks_);
    else        run(bk.bids_);
}



// do_submit — single internal path for all order types
//
// Phase 4 Change 3: add_order() logic absorbed directly here.
// All of match(), make_exec(), and do_submit() are now in the same TU —
// the compiler can inline the full order lifecycle into one function body.
//
// Sequence:
//   1. Resolve book + guard                     (array read, O(1))
//   2. Price bounds check (limit only)
//   3. Start timer, bump order_seq
//   4. FOK pre-check via simulate match         (read-only book walk)
//   5. Pool alloc + real match                  (fills → trade_ring_)
//   6. Post-match TIF handling (rest / cancel)
//   7. Record latency → bk.stats_ + engine stats_

OrderResult MatchingEngine::do_submit(SymbolId  sid,
                                      Side      side,
                                      Price     price,
                                      Quantity  qty,
                                      OrderType type,
                                      TIF       tif)
{
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

    SymbolState& ss = symbols_[sid];   // sid validated by get_book above
    {

        // Price bounds check — limit orders only.
        // Market orders use MARKET_PRICE sentinel; skip.
        if (type == OrderType::Limit) {
            const Price base = bk->bids_.base_tick_;
            if (price < base ||
                static_cast<uint32_t>(price - base) >= BookSide<true>::MAX_TICKS)
            {
                throw std::out_of_range(
                    "price outside book window [base_tick, base_tick + MAX_TICKS)");
            }
        }

        const OrderId id = next_id_++;
        ++stats_.orders_received;

        // RDTSC: ~3 cycles vs ~23 ns for Clock::now() — saves ~40 ns per order.
        const std::uint64_t t0 = rdtsc();
        ++ss.order_seq;

        // Clear once — match() writes fills here; simulate path does not touch it.
        trade_ring_.clear();

        // FOK pre-check — simulate fill on a stack probe, no book mutations.
        if (tif == TIF::FOK && type == OrderType::Limit) {
            Order probe(id, price, qty, side, type, tif, ss.order_seq);
            match(*bk, ss, probe, /*simulate_only=*/true);
            if (probe.remaining_qty > 0) {
                bk->stats_.record(
                    static_cast<std::uint64_t>((rdtsc() - t0) * g_ns_per_tick));
                record_latency(bk->stats_.last_ns());
                ++stats_.orders_rejected;
                OrderResult r;
                r.order_id      = id;
                r.accepted      = false;
                r.reject_reason = "FOK: insufficient liquidity";
                if (on_reject_) on_reject_(id, r.reject_reason);
                return r;
            }
        }

        // Allocate from pool and run the real match.
        Order* o = bk->pool_.allocate(id, price, qty, side, type, tif, ss.order_seq);
        match(*bk, ss, *o, /*simulate_only=*/false);

        // Post-match TIF handling
        if (!o->is_active()) {
            // Fully filled — return pool slot immediately.
            bk->pool_.deallocate(o);
        } else if (type == OrderType::Market ||
                   tif  == TIF::IOC         ||
                   tif  == TIF::FOK)
        {
            // Market / IOC / FOK: cancel remaining; never rests.
            o->cancel();
            bk->pool_.deallocate(o);
        } else {
            // GTC Limit: rest the unfilled remainder on the correct side.
            auto rest = [&](auto& side_book) {
                const uint32_t idx = side_book.find_or_insert_idx(price);
                side_book.levels_[idx].push_back(o);
                bk->index_.insert(id, idx, o);
            };
            if (side == Side::Buy) rest(bk->bids_);
            else                   rest(bk->asks_);
        }

        bk->stats_.record(
            static_cast<std::uint64_t>((rdtsc() - t0) * g_ns_per_tick));
        const std::uint64_t elapsed_ns = bk->stats_.last_ns();
        record_latency(elapsed_ns);
        accumulate_trades(trade_ring_);

        const bool fok_rejected = (tif == TIF::FOK) && trade_ring_.empty();

        OrderResult result;
        result.order_id = id;
        if (!trade_ring_.empty()) result.trades = trade_ring_.to_vector();

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
            if (filled == qty)  ++stats_.orders_filled;
            else if (filled > 0) ++stats_.orders_partial;
        }

        return result;
    }  // end inner block
}



// Public submission API — SymbolId overloads (hot path)


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

    // Phase 4 Change 3: cancel_order logic moved here from OrderBook.
    // Direct friend access to index_ and the correct BookSide::cancel().
    Locator* loc = bk->index_.find(id);
    if (!loc) return false;

    const bool cancelled = (loc->order->side == Side::Buy)
        ? bk->bids_.cancel(id)
        : bk->asks_.cancel(id);

    if (cancelled) ++stats_.orders_cancelled;
    return cancelled;
}



// Public submission API — string wrappers (off hot path)


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

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
// OrderBook constructor
OrderBook::OrderBook(std::string   symbol,
                     TradeCallback on_trade,
                     std::size_t   pool_size,
                     std::size_t   /*level_capacity*/,   // unused — flat array needs no hint
                     Price         base_tick)
    : symbol_   (std::move(symbol))
    , on_trade_ (std::move(on_trade))
    , pool_     (pool_size)
    , index_    (pool_size)
    , bids_     (index_, pool_, base_tick)
    , asks_     (index_, pool_, base_tick)
{}
// Internal helpers
Trade OrderBook::make_trade(Order& buy, Order& sell, Price px, Quantity qty)
{
    ++trade_seq_;
    Trade t{
        .buy_order_id  = buy.id,
        .sell_order_id = sell.id,
        .price         = px,
        .quantity      = qty,
        .trade_seq     = trade_seq_,
        // ts only stamped when a callback needs it — Clock::now() costs 23 ns.
        .ts            = on_trade_ ? std::chrono::steady_clock::now() : TimePoint{},
    };
    if (on_trade_) on_trade_(t);
    return t;
}
// match — core matching engine (price-time priority)
//
// Hot path for real fills:
//   side_book.has_best_          — flag check, no indirection
//   lvl.front() / lvl.unlink()  — intrusive list, no alloc
//   erase_level()                — flag flip + advance_best() scan
//
// FOK simulate path (simulate_only == true):
//   BookSide::simulate_fill() — read-only walk of active levels,
//   no book mutations, aggressor.remaining_qty decremented in place.
std::vector<Trade> OrderBook::match(Order& aggressor, bool simulate_only)
{
    std::vector<Trade> trades;
    const bool is_buy = aggressor.side == Side::Buy;
    // prices_cross: true when aggressor crosses the resting price.
    auto prices_cross = [&](Price resting_px) noexcept -> bool {
        if (aggressor.type == OrderType::Market) return true;
        return is_buy ? (aggressor.price >= resting_px)
                      : (aggressor.price <= resting_px);
    };
    if (simulate_only) {
        // FOK pre-check — no book mutations, just decrement remaining_qty.
        // A buy aggressor sweeps asks_; a sell aggressor sweeps bids_.
        if (is_buy) asks_.simulate_fill(aggressor, prices_cross);
        else        bids_.simulate_fill(aggressor, prices_cross);
        return trades;   // always empty for simulate_only
    }
    // Real match path
    // Templated lambda so the compiler sees the concrete BookSide<IsBid> type
    // and can inline erase_level / advance_best with the correct direction.
    auto run = [&](auto& side_book) {
        while (aggressor.is_active() && side_book.has_best_) {
            // Single array access — no sorted vector, no hash lookup.
            PriceLevel& lvl = side_book.levels_[side_book.best_idx_];
            if (!prices_cross(lvl.price)) break;
            // Fill loop: drain this level FIFO until aggressor is filled
            // or the level empties.
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
                trades.push_back(make_trade(buy_o, sell_o, fill_px, fill_qty));
                if (!resting->is_active()) {
                    index_.erase(resting->id);
                    lvl.unlink(resting);
                    pool_.deallocate(resting);
                }
            }
            // Level drained — flip flag and advance best_idx_.
            if (lvl.empty()) {
                side_book.erase_level(side_book.best_idx_);
            }
        }
    };
    if (is_buy) run(asks_);
    else        run(bids_);
    return trades;
}
// add_order — public entry point
std::vector<Trade> OrderBook::add_order(OrderId   id,
                                         Price     price,
                                         Quantity  qty,
                                         Side      side,
                                         OrderType type,
                                         TIF       tif)
{
    if (qty == 0)
        throw std::invalid_argument("order quantity must be > 0");
    // Bounds check for limit orders: price must fall within the tick window.
    // Market orders use MARKET_PRICE sentinel — skip the check.
    if (type == OrderType::Limit) {
        const Price base = bids_.base_tick_;
        if (price < base ||
            static_cast<uint32_t>(price - base) >= BookSide<true>::MAX_TICKS)
        {
            throw std::out_of_range(
                "price outside book window [base_tick, base_tick + MAX_TICKS)");
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    ++seq_;   // sequence number assigned before anything else
    // FOK pre-check
    // Simulate the fill on a stack-allocated probe.  If the order cannot fill
    // completely, reject immediately without touching the book.
    if (tif == TIF::FOK && type == OrderType::Limit) {
        Order probe(id, price, qty, side, type, tif, seq_);
        match(probe, /*simulate_only=*/true);
        if (probe.remaining_qty > 0) {
            stats_.record(std::chrono::steady_clock::now() - t0);
            return {};   // empty vector signals FOK rejection to the caller
        }
    }
    // Allocate order from pool 
    Order* o = pool_.allocate(id, price, qty, side, type, tif, seq_);
    //  Match
    std::vector<Trade> trades = match(*o, /*simulate_only=*/false);
    // Post-match TIF handling 
    if (!o->is_active()) {
        // Fully filled — return pool slot immediately.
        pool_.deallocate(o);
    } else if (type == OrderType::Market ||
               tif  == TIF::IOC         ||
               tif  == TIF::FOK)
    {
        // Market / IOC / FOK: cancel remaining — never rests in book.
        o->cancel();
        pool_.deallocate(o);
    } else {
        // GTC Limit: rest the unfilled remainder on the correct side.
        //
        // find_or_insert_idx: O(1) — tick_to_idx + init_slot if new level.
        // Returns tick_idx stored in Locator so cancel() can reach the level
        // directly without any secondary lookup.
        auto rest = [&](auto& side_book) {
            const uint32_t idx = side_book.find_or_insert_idx(price);
            side_book.levels_[idx].push_back(o);
            index_.insert(id, idx, o);
        };
        if (side == Side::Buy) rest(bids_);
        else                   rest(asks_);
    }
    stats_.record(std::chrono::steady_clock::now() - t0);
    return trades;
}
// cancel_order
bool OrderBook::cancel_order(OrderId id)
{
    Locator* loc = index_.find(id);
    if (!loc) return false;
    // Route to correct side using the Order's own Side field.
    // BookSide::cancel() does the O(1) array lookup via loc->tick_idx.
    if (loc->order->side == Side::Buy)
        return bids_.cancel(id);
    else
        return asks_.cancel(id);
}
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

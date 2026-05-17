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

OrderBook::OrderBook(std::string symbol, TradeCallback on_trade, std::size_t pool_size)
    : symbol_(std::move(symbol))
    , on_trade_(std::move(on_trade))
    , pool_(pool_size)
    , index_(pool_size)
    , bids_(index_, pool_)
    , asks_(index_, pool_)
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
        .ts            = std::chrono::steady_clock::now(),
    };
    if (on_trade_) on_trade_(t);
    return t;
}

// Core matching engine
// Price-time priority.

std::vector<Trade> OrderBook::match(Order& aggressor, bool simulate_only)
{
    std::vector<Trade> trades;

    const bool is_buy = aggressor.side == Side::Buy;

    // Determine whether two prices cross.
    auto prices_cross = [&](Price resting_px) noexcept -> bool {
        if (aggressor.type == OrderType::Market) return true;
        return is_buy ? (aggressor.price >= resting_px)
                      : (aggressor.price <= resting_px);
    };

    // We template the inner loop to avoid a branch per iteration and to let the compiler see the exact LevelMap type.
    auto run = [&](auto& side_book) {
        auto& level_map = side_book.levels();

        while (aggressor.is_active() && !level_map.empty()) {
            PriceLevel& lvl = level_map[0];   // best price always at front

            if (!prices_cross(lvl.price)) break;

            if (simulate_only) {
                // READ-ONLY walk — never touch real book state
                Order* cursor = lvl.front();
                while (aggressor.remaining_qty > 0 && cursor != nullptr) {
                    Quantity consumed = std::min(aggressor.remaining_qty,
                                                cursor->remaining_qty);
                    aggressor.remaining_qty -= consumed;
                    cursor = cursor->next;  // advance via intrusive pointer
                }
                // Move to next price level
                if (aggressor.remaining_qty > 0)
                    break; // no more liquidity at crossing prices will help
                else
                    break; // aggressor fully simulated
            }

            // Real match path (simulate_only == false) — unchanged
            while (aggressor.is_active() && !lvl.empty()) {
                Order* resting   = lvl.front();
                assert(resting && resting->is_active());

                Quantity fill_qty = std::min(aggressor.remaining_qty,
                                             resting->remaining_qty);
                Price    fill_px  = resting->price;

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

            if (lvl.empty())
                side_book.erase_level(lvl.price);
        }
    };

    if (is_buy)  run(asks_);
    else         run(bids_);

    return trades;
}

// add_order ,  the public entry point

std::vector<Trade> OrderBook::add_order(OrderId   id,
                                         Price     price,
                                         Quantity  qty,
                                         Side      side,
                                         OrderType type,
                                         TIF       tif)
{
    if (qty == 0)
        throw std::invalid_argument("order quantity must be > 0");

    // Record when this order entered the engine for latency tracking.
    const auto t0 = std::chrono::steady_clock::now();

    ++seq_;  // Assign sequence number before anything else.

    // FOK pre-check
    
    if (tif == TIF::FOK && type == OrderType::Limit) {
        // Temporarily create a probe order on the stack — not pool-allocated.
        Order probe(id, price, qty, side, type, tif, seq_);
        match(probe, /*simulate_only=*/true);
        if (probe.remaining_qty > 0) {
            // Cannot fill fully — reject without touching the book.
            const auto t1 = std::chrono::steady_clock::now();
            stats_.record(t1 - t0);
            return {};  // Empty trades vector signals rejection to the caller.
        }
    }

    // Allocate the real order from the pool
    // We must allocate before matching so the Order object lives in stable
   
    Order* o = pool_.allocate(id, price, qty, side, type, tif, seq_);

    // Match
    std::vector<Trade> trades = match(*o, /*simulate_only=*/false);

    // Post-match TIF handling 
    if (!o->is_active()) {
        // Fully filled , free the slot immediately (will not rest in book).
        pool_.deallocate(o);
    } else if (type == OrderType::Market || tif == TIF::IOC || tif == TIF::FOK) {
        // Market / IOC / FOK: cancel whatever's left — never rest in book.
        o->cancel();
        pool_.deallocate(o);
    } else {
        // GTC Limit: rest the unfilled remainder in the correct side.
        auto rest = [&](auto& side_book) {
            PriceLevel& lvl = side_book.find_or_insert(price);
            lvl.push_back(o);
            index_.insert(id, price, o);
        };

        if (side == Side::Buy) rest(bids_);
        else                   rest(asks_);
    }

    // Latency accounting
    const auto t1 = std::chrono::steady_clock::now();
    stats_.record(t1 - t0);

    return trades;
}

// cancel_order
bool OrderBook::cancel_order(OrderId id)
{
    // The OrderIndex tells us which side the order is on in O(1).
    Locator* loc = index_.find(id);
    if (!loc) return false;

    // Dispatch to the correct side.
    // The Order itself stores its Side so we can route in one branch.
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
    return (*bid + *ask) / 2;   // Integer arithmetic — no FP rounding.
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
              << std::setw(14) << "ORDERS"
              << "\n";

    // Print asks top-down (highest ask first).
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::cout << "  ASK"
                  << std::setw(14) << it->first
                  << std::setw(14) << it->second
                  << "\n";

    if (auto sp = spread())
        std::cout << "   spread " << *sp << " ticks \n";

    for (auto& [p, q] : bids)
        std::cout << "  BID"
                  << std::setw(14) << p
                  << std::setw(14) << q
                  << "\n";

    auto perc = stats_.compute();
    std::cout << "\nLatency (ns) | orders=" << order_count()
              << " trades=" << trade_count() << "\n"
              << "  min=" << perc.min
              << " p50=" << perc.p50
              << " p99=" << perc.p99
              << " p999=" << perc.p999
              << " max=" << perc.max << "\n\n";
}

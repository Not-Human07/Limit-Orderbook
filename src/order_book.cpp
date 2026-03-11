#include "order_book.hpp"

#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cassert>

// BookSide

template <typename Comp>
void BookSide<Comp>::add(std::unique_ptr<Order> order)
{
    Price    p  = order->price();
    OrderId  id = order->id();

    // Create the level if it doesn't exist yet.
    auto level_it = levels_.find(p);
    if (level_it == levels_.end()) {
        auto [it, ok] = levels_.emplace(p, PriceLevel{ .price = p });
        level_it = it;
    }

    PriceLevel& lvl = level_it->second;
    lvl.total_qty  += order->remaining_qty();
    lvl.orders.push_back(std::move(order));

    auto order_it = std::prev(lvl.orders.end());
    index_.emplace(id, Locator{ level_it, order_it });
}

template <typename Comp>
bool BookSide<Comp>::cancel(OrderId id)
{
    auto idx = index_.find(id);
    if (idx == index_.end())
        return false;

    auto& [level_it, order_it] = idx->second;
    PriceLevel& lvl = level_it->second;

    Order& o = **order_it;
    if (!o.is_active()) {
        index_.erase(idx);
        return false;
    }

    lvl.total_qty -= o.remaining_qty();
    o.cancel();
    lvl.orders.erase(order_it);

    if (lvl.empty())
        levels_.erase(level_it);

    index_.erase(idx);
    return true;
}

template <typename Comp>
std::vector<std::pair<Price, Quantity>> BookSide<Comp>::depth(std::size_t n) const
{
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(n);

    for (auto& [price, lvl] : levels_) {
        if (out.size() == n) break;
        out.emplace_back(price, lvl.total_qty);
    }
    return out;
}

// Explicit instantiations — keeps the linker happy without moving all this into the header.
template class BookSide<std::greater<Price>>;
template class BookSide<std::less<Price>>;

// OrderBook

OrderBook::OrderBook(std::string symbol, TradeCallback on_trade)
    : symbol_(std::move(symbol))
    , on_trade_(std::move(on_trade))
{}

// Matching logic

// Price-time priority:
//   1. Walk the opposite side level by level (best price first).
//   2. Within a level, drain orders front-to-back (time priority).
//   3. Stop when the aggressor is fully filled or there's no crossable price.

std::vector<Trade> OrderBook::match(Order& aggressor)
{
    std::vector<Trade> trades;

    bool is_buy = aggressor.side() == Side::Buy;

    // Lambda to decide whether two prices cross.
    auto prices_cross = [&](Price resting_price) -> bool {
        if (aggressor.type() == OrderType::Market)
            return true;
        return is_buy ? (aggressor.price() >= resting_price)
                      : (aggressor.price() <= resting_price);
    };

    auto& opposite = is_buy ? static_cast<BookSide<std::less<Price>>&>(asks_)
                             : static_cast<BookSide<std::greater<Price>>&>(bids_);

    while (aggressor.is_active() && !opposite.empty()) {
        PriceLevel* lvl = opposite.best();
        if (!lvl || !prices_cross(lvl->price))
            break;

        // Drain the level front-to-back.
        while (aggressor.is_active() && !lvl->empty()) {
            Order& resting = *lvl->front();
            assert(resting.is_active());

            Quantity fill_qty = std::min(aggressor.remaining_qty(), resting.remaining_qty());
            Price    fill_px  = resting.price(); // resting order sets the price

            aggressor.fill(fill_qty);
            resting.fill(fill_qty);

            lvl->total_qty -= fill_qty;

            Trade t = is_buy
                ? Trade{ aggressor.id(), resting.id(), fill_px, fill_qty }
                : Trade{ resting.id(), aggressor.id(), fill_px, fill_qty };

            if (on_trade_) on_trade_(t);
            trades.push_back(std::move(t));

            // If the resting order is done, pop it.
            if (!resting.is_active())
                lvl->orders.pop_front();
        }

        // Clean up the empty level.
        if (lvl->empty()) {
            auto& map = opposite.levels();
            // const_cast is fine — we own the book.
            const_cast<std::remove_reference_t<decltype(map)>&>(map)
                .erase(lvl->price);
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::add_order(std::unique_ptr<Order> order)
{
    if (!order)
        throw std::invalid_argument("null order");

    std::vector<Trade> trades;

    // Market orders must find liquidity immediately; if the book is dry after
    // matching we just let the remaining quantity die — no resting market orders.
    if (order->type() == OrderType::Market) {
        trades = match(*order);
        return trades;   // intentionally not resting leftover market qty
    }

    // Limit order: match first, then rest any unfilled remainder.
    trades = match(*order);

    if (order->is_active()) {
        if (order->side() == Side::Buy)
            bids_.add(std::move(order));
        else
            asks_.add(std::move(order));
    }

    return trades;
}

bool OrderBook::cancel_order(OrderId id)
{
    // Try bids first, then asks.  Only one side will have it.
    return bids_.cancel(id) || asks_.cancel(id);
}

// Queries

std::optional<Price> OrderBook::best_bid() const
{
    if (bids_.empty()) return std::nullopt;
    return bids_.best_price();
}

std::optional<Price> OrderBook::best_ask() const
{
    if (asks_.empty()) return std::nullopt;
    return asks_.best_price();
}

std::optional<Price> OrderBook::mid_price() const
{
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return (*bid + *ask) / 2.0;
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
    std::cout << std::setw(12) << "PRICE"
              << std::setw(12) << "QTY"
              << "\n";

    // Print asks top-down (highest ask first for readability).
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::cout << "  ASK " << std::setw(10) << it->first
                  << std::setw(10) << it->second << "\n";

    if (auto sp = spread())
        std::cout << "  --- spread " << *sp << " ---\n";

    for (auto& [p, q] : bids)
        std::cout << "  BID " << std::setw(10) << p
                  << std::setw(10) << q << "\n";

    std::cout << std::endl;
}
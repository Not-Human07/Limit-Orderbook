#pragma once

#include "order.hpp"

#include <map>
#include <list>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>
#include <functional>

// PriceLevel
// One row in the book — all orders resting at the same price, in FIFO order.
// We keep a raw total so the engine never has to walk the queue for depth data.

struct PriceLevel {
    Price                         price;
    Quantity                      total_qty { 0 };
    std::list<std::unique_ptr<Order>> orders;

    // Convenience: front of the queue is always the oldest resting order.
    Order* front() const { return orders.empty() ? nullptr : orders.front().get(); }
    bool   empty() const { return orders.empty(); }
};

// BookSide
//
// One half of the book (all bids or all asks).
//
// Bids  → sorted descending  (highest price first — best bid at top)
// Asks  → sorted ascending   (lowest  price first — best ask at top)
//
// Comparator is a template param so we get one compiled type per side
// with zero virtual dispatch.

template <typename Comp>
class BookSide {
public:
    using LevelMap = std::map<Price, PriceLevel, Comp>;

    void     add(std::unique_ptr<Order> order);
    bool     cancel(OrderId id);

    // Returns nullptr if the side is empty.
    PriceLevel*       best()       { return levels_.empty() ? nullptr : &levels_.begin()->second; }
    const PriceLevel* best() const { return levels_.empty() ? nullptr : &levels_.begin()->second; }

    bool  empty()       const { return levels_.empty(); }
    Price best_price()  const { return levels_.empty() ? Price{0} : levels_.begin()->first; }

    // Snapshot of (price → total_qty) for the top N levels — used for depth feeds.
    std::vector<std::pair<Price, Quantity>> depth(std::size_t n) const;

    const LevelMap& levels() const { return levels_; }

private:
    LevelMap levels_;

    // Fast lookup: order id → iterator into the level's list.
    // Lets cancel() run in O(1) rather than walking the book.
    struct Locator {
        typename LevelMap::iterator                        level_it;
        typename std::list<std::unique_ptr<Order>>::iterator order_it;
    };
    std::unordered_map<OrderId, Locator> index_;
};

// OrderBook

class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(std::string symbol, TradeCallback on_trade = nullptr);

    // Returns all trades generated (could be many for a market order).
    std::vector<Trade> add_order(std::unique_ptr<Order> order);
    bool               cancel_order(OrderId id);

    // Best prices — nullopt when that side is empty.
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> mid_price() const;
    std::optional<Price> spread()    const;

    // Top-N depth on each side.
    std::vector<std::pair<Price, Quantity>> bid_depth(std::size_t n = 5) const;
    std::vector<std::pair<Price, Quantity>> ask_depth(std::size_t n = 5) const;

    const std::string& symbol() const { return symbol_; }

    void print_top(std::size_t levels = 5) const;

private:
    std::string symbol_;
    TradeCallback on_trade_;

    // Bids: descending price.  Asks: ascending price.
    BookSide<std::greater<Price>> bids_;
    BookSide<std::less<Price>>    asks_;

    std::vector<Trade> match(Order& aggressor);
};
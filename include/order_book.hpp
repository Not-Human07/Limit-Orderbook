#pragma once

// order.hpp is the single source of truth for all primitive types,
// Order, Trade, TradeCallback, enums, MARKET_PRICE, PRICE(), price_to_double().
// Everything order_book.hpp needs from those is pulled in transitively.
#include "order.hpp"

// std headers needed by order_book.hpp itself (not already in order.hpp)
#include <array>
#include <cassert>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


// Pool Allocator  (slab-style, fixed-size objects, cache-line aligned)

// Alignment is 64 bytes (one cache line) to avoid false sharing.

template <typename T, std::size_t N = 1 << 20>
class PoolAllocator {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    explicit PoolAllocator(std::size_t capacity = N) : capacity_(capacity) {
        // Lazy: reserve memory but don't touch pages until first alloc.
        // slab_ grows on demand; free_list_ only has indices of returned slots.
        slab_.reserve(std::min(capacity, N));
        free_list_.reserve(256);  // Small initial reservation , grows as needed.
    }

    template <typename... Args>
    T* allocate(Args&&... args) {
        if (!free_list_.empty()) {
            std::size_t idx = free_list_.back();
            free_list_.pop_back();
            return new (&slab_[idx]) T(std::forward<Args>(args)...);
        }
        if (slab_.size() >= capacity_) [[unlikely]]
            throw std::bad_alloc{};
        slab_.emplace_back();  // Extend slab by one Storage slot.
        std::size_t idx = slab_.size() - 1;
        return new (&slab_[idx]) T(std::forward<Args>(args)...);
    }

    void deallocate(T* p) noexcept {
        if (!p) return;
        p->~T();
        std::size_t idx = static_cast<std::size_t>(
            reinterpret_cast<Storage*>(p) - slab_.data());
        free_list_.push_back(idx);
    }

    std::size_t capacity()  const noexcept { return capacity_; }
    std::size_t available() const noexcept {
        return capacity_ - slab_.size() + free_list_.size();
    }

private:
    struct alignas(64) Storage { std::byte data[sizeof(T)]; };

    std::size_t              capacity_;
    std::vector<Storage>     slab_;
    std::vector<std::size_t> free_list_;
};


// PriceLevel , intrusive doubly-linked list of Orders at one price
struct PriceLevel {
    Price    price{0};
    Quantity total_qty{0};
    Order*   head{nullptr};   ///< Oldest (highest priority) order.
    Order*   tail{nullptr};   ///< Newest order.
    std::size_t order_count{0};

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    /// Append to tail (time-priority: newer orders go to back).
    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        ++order_count;
        total_qty += o->remaining_qty;
    }

    /// Unlink an arbitrary node — O(1).
    void unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail = o->prev;
        o->prev = o->next = nullptr;
        --order_count;
    }

    Order* front() noexcept { return head; }
    void   pop_front() noexcept { if (head) unlink(head); }
};

// OrderIndex , O(1) amortised lookup: OrderId → (Price, Order*)

struct Locator {
    Price  level_price{0};   // ← price instead of PriceLevel*
    Order* order{nullptr};
};

// Reserve enough buckets up front to keep load factor low and avoid rehash
// during a benchmark run.
class OrderIndex {
public:
    explicit OrderIndex(std::size_t expected_orders = 1 << 20) {
        map_.reserve(expected_orders);
    }

    void insert(OrderId id, Price level_price, Order* o) {
        map_.emplace(id, Locator{level_price, o});
    }

    [[nodiscard]] Locator* find(OrderId id) {
        auto it = map_.find(id);
        return (it == map_.end()) ? nullptr : &it->second;
    }

    void erase(OrderId id) { map_.erase(id); }

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

private:
    std::unordered_map<OrderId, Locator> map_;
};

// BookSide — one side of the order book (bids or asks)
// Uses a flat sorted vector of PriceLevels instead of std::map.
// Best price is always levels_[0].
// Insert/erase are O(N) shifts but N is tiny in practice (< 50 levels).
// Cache locality dominates — 3-5x faster than std::map on hot path.

template <bool IsBid>   // true = bids (descending), false = asks (ascending)
class BookSide {
public:
    // LevelMap is now a vector — kept as a type alias so order_book.cpp
    // can use auto& level_map = side_book.levels() unchanged.
    using LevelMap = std::vector<PriceLevel>;

    explicit BookSide(OrderIndex& index, PoolAllocator<Order>& pool)
        : index_(index), pool_(pool)
    { levels_.reserve(256); }  // Pre-allocate 256 price levels for stability

    // Mutating operations
    bool cancel(OrderId id);

    // Non-mutating queries
    [[nodiscard]] bool        empty()      const noexcept { return levels_.empty(); }
    [[nodiscard]] PriceLevel* best()       noexcept { return levels_.empty() ? nullptr : &levels_[0]; }
    [[nodiscard]] Price       best_price() const    { assert(!levels_.empty()); return levels_[0].price; }

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>> depth(std::size_t n) const;

    // Direct vector access for the matching engine.
    LevelMap&       levels() noexcept       { return levels_; }
    const LevelMap& levels() const noexcept { return levels_; }

    // Find or insert a price level, maintaining sorted order.
    // Returns reference to the level (stable until next insert/erase).
    PriceLevel& find_or_insert(Price px);

    // Erase an empty level by price.
    void erase_level(Price px);

    friend class OrderBook;

private:
    // Returns iterator to the level with this price, or end() if not found.
    // Bids: descending (highest first). Asks: ascending (lowest first).
    typename LevelMap::iterator find_level(Price px);

    LevelMap              levels_;
    OrderIndex&           index_;
    PoolAllocator<Order>& pool_;
};

// Latency statistics (lock-free ring buffer of nanosecond samples)

struct LatencyStats {
    static constexpr std::size_t kBuckets = 1 << 14;  // 16 384 samples

    std::array<std::uint64_t, kBuckets> samples{};
    std::size_t write_pos{0};
    std::size_t count{0};

    void record(std::chrono::nanoseconds ns) noexcept {
        samples[write_pos & (kBuckets - 1)] =
            static_cast<std::uint64_t>(ns.count());
        ++write_pos;
        if (count < kBuckets) ++count;
    }

    /// Returns {min, p50, p99, p999, max} in nanoseconds.
    /// Sorts a copy — call only offline (not on the hot path).
    struct Percentiles { std::uint64_t min, p50, p99, p999, max; };
    [[nodiscard]] Percentiles compute() const;
};

// OrderBook

class OrderBook {
public:
    explicit OrderBook(std::string symbol,
                       TradeCallback on_trade   = nullptr,
                       std::size_t   pool_size  = 1 << 20);

    // Lifecycle

    /**
     * Submit a new order.
     * Returns the list of trades generated.
     * FOK: returns empty vector + rejects the order if it can't fill fully.
     * IOC: fills what it can, cancels the rest (never rests in book).
     * GTC: fills what it can, rests remainder.
     */
    std::vector<Trade> add_order(OrderId   id,
                                  Price     price,
                                  Quantity  qty,
                                  Side      side,
                                  OrderType type = OrderType::Limit,
                                  TIF       tif  = TIF::GTC);

   
    bool cancel_order(OrderId id);

    // Queries

    [[nodiscard]] std::optional<Price> best_bid()  const;
    [[nodiscard]] std::optional<Price> best_ask()  const;
    [[nodiscard]] std::optional<Price> mid_price() const;
    [[nodiscard]] std::optional<Price> spread()    const;

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>> bid_depth(std::size_t n = 10) const;
    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>> ask_depth(std::size_t n = 10) const;

    [[nodiscard]] std::uint64_t order_count()  const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t trade_count()  const noexcept { return trade_seq_; }
    [[nodiscard]] const LatencyStats& latency_stats() const noexcept { return stats_; }

    // Debug 
    void print_top(std::size_t levels = 5) const;

private:
    // Core matching loop.  Returns filled trades.
    // simulate_only=true: counts potential fills without committing (for FOK check).
    std::vector<Trade> match(Order& aggressor, bool simulate_only = false);

    // Convenience: make a Trade struct and fire the callback.
    Trade make_trade(Order& buy, Order& sell, Price px, Quantity qty);

    std::string                  symbol_;
    TradeCallback                on_trade_;

    PoolAllocator<Order>         pool_;
    OrderIndex                   index_;

    BookSide<true>  bids_;   // IsBid=true  → descending, best bid = levels_[0]
    BookSide<false> asks_;   // IsBid=false → ascending,  best ask = levels_[0]

    SeqNum   seq_{0};        ///< Increments per accepted order.
    SeqNum   trade_seq_{0};  ///< Increments per trade.

    LatencyStats stats_;
};

// BookSide method implementations

template <bool IsBid>
typename BookSide<IsBid>::LevelMap::iterator
BookSide<IsBid>::find_level(Price px)
{
    // Binary search — O(log N) but N is tiny so this is just a few comparisons.
    auto it = std::lower_bound(
        levels_.begin(), levels_.end(), px,
        [](const PriceLevel& lvl, Price p) {
            if constexpr (IsBid)
                return lvl.price > p;   // descending: find first where price <= px
            else
                return lvl.price < p;   // ascending:  find first where price >= px
        });
    if (it != levels_.end() && it->price == px) return it;
    return levels_.end();
}

template <bool IsBid>
PriceLevel& BookSide<IsBid>::find_or_insert(Price px)
{
    // Find insertion point maintaining sorted order.
    auto it = std::lower_bound(
        levels_.begin(), levels_.end(), px,
        [](const PriceLevel& lvl, Price p) {
            if constexpr (IsBid)
                return lvl.price > p;
            else
                return lvl.price < p;
        });

    if (it != levels_.end() && it->price == px)
        return *it;  // Already exists — return it.

    // Insert new level at sorted position.
    it = levels_.insert(it, PriceLevel{});
    it->price = px;
    return *it;
}

template <bool IsBid>
void BookSide<IsBid>::erase_level(Price px)
{
    auto it = find_level(px);
    if (it != levels_.end())
        levels_.erase(it);
}

template <bool IsBid>
bool BookSide<IsBid>::cancel(OrderId id)
{
    Locator* loc = index_.find(id);
    if (!loc) return false;

    Order* o = loc->order;
    Price  px = loc->level_price;   // ← safe, just an integer

    if (!o->is_active()) {
        index_.erase(id);
        return false;
    }

    // Look up level fresh — pointer always valid
    auto it = find_level(px);
    if (it == levels_.end()) {
        index_.erase(id);
        return false;
    }
    PriceLevel& lvl = *it;

    lvl.total_qty -= o->remaining_qty;
    o->cancel();
    lvl.unlink(o);
    pool_.deallocate(o);

    if (lvl.empty())
        levels_.erase(it);   // ← erase by iterator directly, no second search

    index_.erase(id);
    return true;
}

template <bool IsBid>
std::vector<std::pair<Price, Quantity>>
BookSide<IsBid>::depth(std::size_t n) const
{
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(std::min(n, levels_.size()));
    for (std::size_t i = 0; i < levels_.size() && i < n; ++i)
        out.emplace_back(levels_[i].price, levels_[i].total_qty);
    return out;
}

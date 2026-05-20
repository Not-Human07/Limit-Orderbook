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
template <typename T, std::size_t N = 1 << 20>
class PoolAllocator {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    explicit PoolAllocator(std::size_t capacity = N) : capacity_(capacity) {
        // BUG 1 FIX: removed std::min(capacity, N) cap — was silently capping
        // reserve at N even when capacity > N, causing bad_alloc or UB on
        // deallocate (slab_.data() changes after realloc).
        slab_.reserve(capacity);
        free_list_.reserve(256);
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
        slab_.emplace_back();
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

// PriceLevel — intrusive doubly-linked list of Orders at one price

struct PriceLevel {
    Price    price{0};
    Quantity total_qty{0};
    Order*   head{nullptr};
    Order*   tail{nullptr};
    std::size_t order_count{0};

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        ++order_count;
        total_qty += o->remaining_qty;
    }

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

// OrderIndex — O(1) lookup: OrderId → (slab_idx, Order*)
//
// Locator now stores a uint16_t slab_idx directly into BookSide's
// LevelSlab.  cancel() uses this to reach the PriceLevel in O(1)
// with zero hash lookups and zero binary searches.
//
// uint16_t supports up to 65535 simultaneous price levels — more
// than enough for any realistic book.  Fits in a register.

static constexpr uint16_t kInvalidSlabIdx = 0xFFFF;

struct Locator {
    uint16_t slab_idx{kInvalidSlabIdx};  // direct index into BookSide::slab_
    Order*   order{nullptr};
};

class OrderIndex {
public:
    explicit OrderIndex(std::size_t expected_orders = 1 << 20) {
        map_.reserve(expected_orders);
    }

    void insert(OrderId id, uint16_t slab_idx, Order* o) {
        map_.emplace(id, Locator{slab_idx, o});
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
//
// Architecture: LevelSlab + sorted index vector
//
//   slab_       : vector<PriceLevel>   — contiguous, elements NEVER move.
//                 Slots are recycled via free_slots_ free-list.
//                 Match loop dereferences into this — sequential,
//                 prefetcher-friendly, same cache behaviour as the
//                 original flat vector<PriceLevel>.
//
//   level_map_  : vector<uint16_t>     — sorted slab indices.
//                 Best price at level_map_[0].
//                 Each entry is 2 bytes; 500 entries = 1 KB — fits in L1.
//                 Match loop: level_map_[i] → slab_[level_map_[i]].
//                 Sequential scan of a tiny array; next cache line
//                 prefetched before it's needed.
//
//   px_to_slab_ : unordered_map<Price, uint16_t>
//                 Maps price → slab index.  Used ONLY on the insert
//                 path (find_or_insert) and NOT on the cancel or match
//                 hot paths.  Cancel uses Locator::slab_idx directly.
//
// Performance properties:
//   cancel()        : 1 OrderIndex lookup (warm) → slab_[idx] directly.
//                     Zero binary search.  O(1) always.
//   match loop      : level_map_ scan (uint16_t array, L1-resident) →
//                     slab_ access (contiguous PriceLevels, prefetchable).
//                     Same memory behaviour as original flat vector.
//   find_or_insert  : px_to_slab_ hash lookup on hit (existing level) →
//                     no binary search needed for the push_back call
//                     (slab_idx returned directly). Binary search only
//                     on NEW level insert to find sorted position in
//                     level_map_.  New level inserts are rare vs fills.

template <bool IsBid>
class BookSide {
public:
    // LevelMap exposed to the matching engine.
    // order_book.cpp: auto& level_map = side_book.levels()
    // level_map[i]  → uint16_t slab index
    // slab(level_map[i]) → PriceLevel&   (use side_book.slab(idx))
    using LevelMap = std::vector<uint16_t>;

    explicit BookSide(OrderIndex& index, PoolAllocator<Order>& pool,
                      std::size_t level_capacity = 4096)
        : index_(index), pool_(pool)
    {
        slab_.reserve(level_capacity);
        free_slots_.reserve(64);
        level_map_.reserve(level_capacity);
        px_to_slab_.reserve(level_capacity);
    }

    // accessors used by order_book.cpp

    [[nodiscard]] bool  empty()      const noexcept { return level_map_.empty(); }

    // best() returns a pointer to the best PriceLevel (nullptr if empty).
    [[nodiscard]] PriceLevel* best() noexcept {
        return level_map_.empty() ? nullptr : &slab_[level_map_[0]];
    }

    // best_price() — only call when !empty().
    [[nodiscard]] Price best_price() const {
        assert(!level_map_.empty());
        return slab_[level_map_[0]].price;
    }

    // slab(idx) — gives a PriceLevel& from a slab index.
    // Used by order_book.cpp match loop:
    //   PriceLevel& lvl = side_book.slab(level_map[i]);
    [[nodiscard]] PriceLevel& slab(uint16_t idx) noexcept { return slab_[idx]; }
    [[nodiscard]] const PriceLevel& slab(uint16_t idx) const noexcept { return slab_[idx]; }

    LevelMap&       levels() noexcept       { return level_map_; }
    const LevelMap& levels() const noexcept { return level_map_; }

    // core operations

    bool cancel(OrderId id);

    // Returns slab index for the given price.
    // Inserts a new PriceLevel into slab_ and level_map_ if not present.
    uint16_t find_or_insert(Price px);

    // Remove the level at slab index idx from level_map_ and recycle the slot.
    // Called by order_book.cpp match loop when a level drains to zero.
    void erase_level(uint16_t slab_idx);

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>> depth(std::size_t n) const;

    friend class OrderBook;

private:
    // Binary search in level_map_ for a slab index whose price == px.
    // Returns level_map_.end() if not found.
    typename LevelMap::iterator find_level_iter(Price px);

    // Contiguous slab — PriceLevels live here and never move.
    std::vector<PriceLevel>  slab_;
    // Free-list of recycled slab slots.
    std::vector<uint16_t>    free_slots_;
    // Sorted vector of slab indices: best price first.
    LevelMap                 level_map_;
    // Price → slab index map.  Insert path only.
    std::unordered_map<Price, uint16_t> px_to_slab_;

    OrderIndex&           index_;
    PoolAllocator<Order>& pool_;
};

// LatencyStats

struct LatencyStats {
    static constexpr std::size_t kBuckets = 1 << 14;

    std::array<std::uint64_t, kBuckets> samples{};
    std::size_t write_pos{0};
    std::size_t count{0};

    void record(std::chrono::nanoseconds ns) noexcept {
        samples[write_pos & (kBuckets - 1)] =
            static_cast<std::uint64_t>(ns.count());
        ++write_pos;
        if (count < kBuckets) ++count;
    }

    struct Percentiles { std::uint64_t min, p50, p99, p999, max; };
    [[nodiscard]] Percentiles compute() const;
};

// OrderBook

class OrderBook {
public:
    explicit OrderBook(std::string symbol,
                       TradeCallback on_trade       = nullptr,
                       std::size_t   pool_size      = 1 << 20,
                       std::size_t   level_capacity = 4096);

    std::vector<Trade> add_order(OrderId   id,
                                  Price     price,
                                  Quantity  qty,
                                  Side      side,
                                  OrderType type = OrderType::Limit,
                                  TIF       tif  = TIF::GTC);

    bool cancel_order(OrderId id);

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

    void print_top(std::size_t levels = 5) const;

private:
    std::vector<Trade> match(Order& aggressor, bool simulate_only = false);
    Trade make_trade(Order& buy, Order& sell, Price px, Quantity qty);

    std::string          symbol_;
    TradeCallback        on_trade_;
    PoolAllocator<Order> pool_;
    OrderIndex           index_;
    BookSide<true>       bids_;
    BookSide<false>      asks_;
    SeqNum               seq_{0};
    SeqNum               trade_seq_{0};
    LatencyStats         stats_;
};

// BookSide method implementations

template <bool IsBid>
typename BookSide<IsBid>::LevelMap::iterator
BookSide<IsBid>::find_level_iter(Price px)
{
    // Binary search level_map_ by price stored in slab_.
    auto it = std::lower_bound(
        level_map_.begin(), level_map_.end(), px,
        [this](uint16_t idx, Price p) {
            if constexpr (IsBid) return slab_[idx].price > p;
            else                 return slab_[idx].price < p;
        });
    if (it != level_map_.end() && slab_[*it].price == px) return it;
    return level_map_.end();
}

template <bool IsBid>
uint16_t BookSide<IsBid>::find_or_insert(Price px)
{
    // --- Hit path: level already exists ---
    // One hash lookup → slab index directly.  No binary search.
    // This is the common case on the resting hot path.
    auto pm_it = px_to_slab_.find(px);
    if (pm_it != px_to_slab_.end()) {
        return pm_it->second;
    }

    // Miss path: new level 
    // Allocate a slab slot (recycle free slot or extend).
    uint16_t idx;
    if (!free_slots_.empty()) {
        idx = free_slots_.back();
        free_slots_.pop_back();
        slab_[idx] = PriceLevel{.price = px};   // reinitialise recycled slot
    } else {
        assert(slab_.size() < kInvalidSlabIdx && "Too many simultaneous price levels");
        idx = static_cast<uint16_t>(slab_.size());
        slab_.push_back(PriceLevel{.price = px});
    }

    // Register in price map.
    px_to_slab_.emplace(px, idx);

    // Insert slab index into the sorted level_map_.
    // Binary search is on uint16_t values (2 bytes each) — very cache-friendly.
    auto lv_it = std::lower_bound(
        level_map_.begin(), level_map_.end(), px,
        [this](uint16_t i, Price p) {
            if constexpr (IsBid) return slab_[i].price > p;
            else                 return slab_[i].price < p;
        });
    level_map_.insert(lv_it, idx);

    return idx;
}

template <bool IsBid>
void BookSide<IsBid>::erase_level(uint16_t slab_idx)
{
    Price px = slab_[slab_idx].price;

    // Remove from sorted index vector — shifting uint16_t values (2 bytes).
    auto lv_it = find_level_iter(px);
    if (lv_it != level_map_.end())
        level_map_.erase(lv_it);

    // Remove from price map.
    px_to_slab_.erase(px);

    // Reset the slab slot and mark it free for reuse.
    slab_[slab_idx] = PriceLevel{};
    free_slots_.push_back(slab_idx);
}

template <bool IsBid>
bool BookSide<IsBid>::cancel(OrderId id)
{
    Locator* loc = index_.find(id);
    if (!loc) return false;

    Order*   o        = loc->order;
    uint16_t slab_idx = loc->slab_idx;

    if (!o->is_active()) {
        index_.erase(id);
        return false;
    }

    // Direct slab access — O(1), zero hash lookups, zero binary search.
    // slab_idx was stored in Locator at order insertion time.
    PriceLevel& lvl = slab_[slab_idx];

    lvl.total_qty -= o->remaining_qty;
    o->cancel();
    lvl.unlink(o);
    pool_.deallocate(o);
    index_.erase(id);

    if (lvl.empty()) {
        erase_level(slab_idx);
    }

    return true;
}

template <bool IsBid>
std::vector<std::pair<Price, Quantity>>
BookSide<IsBid>::depth(std::size_t n) const
{
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(std::min(n, level_map_.size()));
    for (std::size_t i = 0; i < level_map_.size() && i < n; ++i)
        out.emplace_back(slab_[level_map_[i]].price,
                         slab_[level_map_[i]].total_qty);
    return out;
}

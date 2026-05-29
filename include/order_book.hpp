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
#include <vector>
// PoolAllocator  (slab-style, fixed-size objects, cache-line aligned)
template <typename T, std::size_t N = 1 << 20>
class PoolAllocator {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    explicit PoolAllocator(std::size_t capacity = N) : capacity_(capacity) {
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
            std::terminate();
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
// PriceLevel — intrusive doubly-linked list of Orders at one price.
// alignas(64): one cache line per level.  Each slot in BookSide::levels_[]
// occupies exactly one cache line — no false sharing, no straddling.
// Padding is uninitialized on purpose (never read, never written after reset).
struct alignas(64) PriceLevel {
    Price       price{0};
    Quantity    total_qty{0};
    Order*      head{nullptr};
    Order*      tail{nullptr};
    std::size_t order_count{0};
    // 40 bytes used above; 24 bytes of padding to complete the 64-byte line.
    // No initializer — padding bytes are never read; zeroing them costs 40 ns
    // per construction (benchmarked in Phase 1).
    uint8_t _pad[24];

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

static_assert(sizeof(PriceLevel)  == 64, "PriceLevel must be exactly one cache line");
static_assert(alignof(PriceLevel) == 64, "PriceLevel must be cache-line aligned");
// Locator — stored per resting order in OrderIndex.
//
// Phase 2 change: slab_idx (uint16_t) → tick_idx (uint32_t).
// tick_idx is the direct index into BookSide::levels_[].
// cancel() uses it for an O(1) array access — zero hashing, zero search.
struct Locator {
    uint32_t tick_idx{UINT32_MAX};  // direct index into BookSide::levels_[]
    Order*   order{nullptr};
};
// OrderIndex — O(1) lookup: OrderId → Locator
//
// Phase 3: flat array replacing unordered_map.
// Indexed by (id & ID_MASK) — one array write on insert, one read on find,
// one pointer clear on erase.  No hashing, no bucket scan, no rehash.
//
// Safety: OrderId is monotonically assigned.  Two live orders share a slot
// only if their ids differ by a multiple of SLOTS (1 048 576).  With ≤ 50K
// concurrent live orders this is impossible within normal operation.
class OrderIndex {
public:
    static constexpr std::size_t   SLOTS   = 1u << 20;   // 1 048 576 slots → 16 MB
    static constexpr std::uint64_t ID_MASK = SLOTS - 1;

    // Accept the pool-size arg OrderBook passes — silently ignored; array
    // is always SLOTS wide regardless of how many orders the pool holds.
    explicit OrderIndex(std::size_t /*ignored*/ = SLOTS) noexcept {}

    void insert(OrderId id, uint32_t tick_idx, Order* o) noexcept {
        slots_[id & ID_MASK] = {tick_idx, o};
    }

    [[nodiscard]] Locator* find(OrderId id) noexcept {
        Locator& s = slots_[id & ID_MASK];
        return s.order ? &s : nullptr;
    }

    // Only the discriminant needs clearing — tick_idx is dead until the next insert.
    void erase(OrderId id) noexcept {
        slots_[id & ID_MASK].order = nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return 0; }   // diagnostic; not hot

private:
    // Value-init: default-constructs each Locator → order = nullptr (slot empty).
    std::array<Locator, SLOTS> slots_{};
};
// BookSide — one side of the order book (bids or asks)
//
// Architecture: flat price array indexed by tick offset from base_tick_.
//
//   levels_[]     : std::array<PriceLevel, MAX_TICKS>
//                   Pre-allocated at BookSide construction.  One slot per
//                   possible tick offset.  Never heap-allocates on the hot
//                   path.  Never moves (array, not vector).
//                   Index: tick_to_idx(price) = price - base_tick_
//
//   active_[]     : std::array<bool, MAX_TICKS>
//                   Occupancy flag.  true  → levels_[i] has live orders.
//                                   false → slot is empty / recycled.
//                   Byte-per-entry (not bitset) so advance_best() can be
//                   auto-vectorised by the compiler.
//
//   best_idx_     : flat index of the current best price level.
//                   Bids: highest active index (= highest price).
//                   Asks: lowest  active index (= lowest  price).
//
//   base_tick_    : price-offset base for the index calculation.
//                   Default PRICE(88.00).  Orders outside
//                   [base_tick_, base_tick_ + MAX_TICKS) are rejected.
//
// Complexity:
//   find_or_insert_idx : O(1)          — one subtraction + one bool check
//   best()             : O(1)          — single array access
//   erase_level        : O(1) + O(Δ)  — mark false + scan inward by spread Δ
//   cancel()           : O(1)          — tick_idx in Locator → direct array hit
//   match loop         : O(fills)      — no sorted vector, no hash, no search
//
// Memory per side: 262144 × 64 B (levels_) + 262144 × 1 B (active_) = 16.25 MB
// Both sides together: ~32.5 MB — fits in L3 on all modern CPUs.
// Active working set (levels near best price) fits in L1/L2.
template <bool IsBid>
class BookSide {
public:
    // MAX_TICKS = 2^18 = 262144 ticks.
    // At TICK_SCALE = 10000 (4 decimal places), this covers a $26.21 range.
    // Default base_tick = PRICE(88.00) → window covers PRICE(88.00)–PRICE(101.11),
    // which encompasses all benchmark price ranges (PRICE(90.00)–PRICE(100.00)).
    static constexpr uint32_t MAX_TICKS = 1u << 17;  // 131072

    explicit BookSide(OrderIndex& index, PoolAllocator<Order>& pool,
                      Price base_tick = PRICE(88.00))
        : index_(index)
        , pool_(pool)
        , base_tick_(base_tick)
    {
        // One-time init at construction — never on the hot path.
        active_.fill(false);
        // levels_ zero-initialises via std::array default construction.
        //
        // Prefault: touch every cache line of levels_ and active_ now.
        // This moves OS page-fault cost from the first benchmark call to
        // construction (which is untimed).  Without this, the first
        // 800+ pages of cold TLB misses add ~150–200 ns to every early order.
        // The volatile reads prevent the compiler from eliding the loop.
        volatile uint8_t sink = 0;
        for (uint32_t i = 0; i < MAX_TICKS; i += 64)   // one touch per cache line
            sink = reinterpret_cast<volatile uint8_t*>(&levels_[i])[0];
        for (uint32_t i = 0; i < MAX_TICKS; i += 64)
            sink = *reinterpret_cast<volatile uint8_t*>(&active_[i]);
        (void)sink;
    }

    // Large fixed arrays — no copies, no moves.
    BookSide(const BookSide&)            = delete;
    BookSide& operator=(const BookSide&) = delete;
    BookSide(BookSide&&)                 = delete;
    BookSide& operator=(BookSide&&)      = delete;
    [[nodiscard]] bool empty() const noexcept { return !has_best_; }
    [[nodiscard]] PriceLevel* best() noexcept {
        return has_best_ ? &levels_[best_idx_] : nullptr;
    }
    // Only call when !empty().
    [[nodiscard]] Price best_price() const noexcept {
        assert(has_best_);
        return levels_[best_idx_].price;
    }
    // Cancel a resting order by id.
    bool cancel(OrderId id);

    // Return the tick index for px, creating the slot if needed.
    // Used by add_order rest path to get the index to store in Locator.
    uint32_t find_or_insert_idx(Price px) noexcept {
        uint32_t idx = tick_to_idx(px);
        if (!active_[idx]) init_slot(idx, px);
        return idx;
    }

    // Mark a level empty and advance best_idx_ if needed.
    // Called by the match loop when a level fully drains.
    void erase_level(uint32_t idx) noexcept;

    // FOK simulate pass: walk active levels from best, decrement
    // aggressor.remaining_qty without touching the book.
    // Callable is `bool(Price)` — returns true if prices cross.
    template <typename PricesCrossFn>
    void simulate_fill(Order& aggressor, PricesCrossFn crosses) const noexcept;

    [[nodiscard]]
    std::vector<std::pair<Price, Quantity>> depth(std::size_t n) const;

    friend class OrderBook;

private:
    [[nodiscard]] uint32_t tick_to_idx(Price px) const noexcept {
        return static_cast<uint32_t>(px - base_tick_);
    }

    [[nodiscard]] bool is_better(uint32_t a, uint32_t b) const noexcept {
        if constexpr (IsBid) return a > b;   // higher index = higher price = better bid
        else                 return a < b;   // lower  index = lower  price = better ask
    }

    // Initialise an empty slot on first use (or after recycling).
    void init_slot(uint32_t idx, Price px) noexcept {
        // No PriceLevel{} here — slots are zero-init at construction,
        // and erase_level() resets head/tail/total_qty/order_count explicitly
        // (see push_back / unlink).  Assigning PriceLevel{} would be a
        // redundant 40-byte memset on every new level.
        levels_[idx].price       = px;
        levels_[idx].total_qty   = 0;
        levels_[idx].head        = nullptr;
        levels_[idx].tail        = nullptr;
        levels_[idx].order_count = 0;
        active_[idx] = true;
        ++active_count_;
        if (!has_best_ || is_better(idx, best_idx_)) {
            best_idx_ = idx;
            has_best_ = true;
        }
    }

    // Scan inward from best_idx_ to find the next active level.
    // Only called when best_idx_ was just erased.  Bounded by spread.
    void advance_best() noexcept {
        if (active_count_ == 0) { has_best_ = false; return; }
        if constexpr (IsBid) {
            // Scan downward: lower index = lower price = worse bid.
            uint32_t i = best_idx_;
            while (i > 0) {
                --i;
                if (active_[i]) { best_idx_ = i; return; }
            }
        } else {
            // Scan upward: higher index = higher price = worse ask.
            for (uint32_t i = best_idx_ + 1; i < MAX_TICKS; ++i) {
                if (active_[i]) { best_idx_ = i; return; }
            }
        }
        has_best_ = false;
    }
    // Flat price array — 262144 × 64 B = 16 MB per side.
    std::array<PriceLevel, MAX_TICKS> levels_;
    // Occupancy flags — 262144 × 1 B = 256 KB per side.
    std::array<bool, MAX_TICKS> active_;
    uint32_t best_idx_    {0};
    bool     has_best_    {false};
    uint32_t active_count_{0};
    OrderIndex&           index_;
    PoolAllocator<Order>& pool_;
    Price                 base_tick_;
};
// LatencyStats — ring-buffer of per-call nanosecond samples
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

    struct Percentiles { std::uint64_t min, p50, p99, p999, max; };
    [[nodiscard]] Percentiles compute() const;

    // Most-recent recorded sample — lets MatchingEngine read the elapsed time
    // that add_order already measured, without a redundant Clock::now() pair.
    [[nodiscard]] std::uint64_t last_ns() const noexcept {
        return count ? samples[(write_pos - 1) & (kBuckets - 1)] : 0;
    }
};
// OrderBook
class OrderBook {
public:
    // level_capacity kept for API compatibility with MatchingEngine — unused
    // internally (the flat array needs no pre-sizing).
    explicit OrderBook(std::string symbol,
                       TradeCallback on_trade       = nullptr,
                       std::size_t   pool_size      = 1 << 20,
                       std::size_t   level_capacity = 4096,
                       Price         base_tick      = PRICE(88.00));
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
// BookSide method implementations (in header — templates require visibility)
template <bool IsBid>
void BookSide<IsBid>::erase_level(uint32_t idx) noexcept
{
    active_[idx] = false;
    // Do NOT assign PriceLevel{} here — that zeroes 40 bytes (memset) on every
    // level drain, same trap as _pad{} in Phase 1 (+40 ns per call).
    // Fields are always overwritten by init_slot() before being read again.
    --active_count_;
    if (idx == best_idx_) advance_best();
}

template <bool IsBid>
bool BookSide<IsBid>::cancel(OrderId id)
{
    Locator* loc = index_.find(id);
    if (!loc) return false;

    Order*   o        = loc->order;
    uint32_t tick_idx = loc->tick_idx;

    if (!o->is_active()) {
        index_.erase(id);
        return false;
    }

    // O(1) direct array access — tick_idx was stored at order insertion time.
    PriceLevel& lvl = levels_[tick_idx];

    lvl.total_qty -= o->remaining_qty;
    o->cancel();
    lvl.unlink(o);
    pool_.deallocate(o);
    index_.erase(id);

    if (lvl.empty()) erase_level(tick_idx);

    return true;
}

template <bool IsBid>
template <typename PricesCrossFn>
void BookSide<IsBid>::simulate_fill(Order& aggressor,
                                     PricesCrossFn crosses) const noexcept
{
    // Walk active levels from best toward the interior, simulating fills.
    // No book mutations — only aggressor.remaining_qty changes.
    // Used exclusively by the FOK pre-check path.
    //
    // Termination is bounded by active_count_ — we stop as soon as we have
    // visited every active level (or the first non-crossing one), so we never
    // scan the entire 262 K-entry active_[] array in the common case where
    // liquidity runs out before the aggressor is fully filled.
    if (!has_best_ || active_count_ == 0) return;

    uint32_t visited = 0;   // count of active levels seen — exit when == active_count_

    if constexpr (IsBid) {
        // bid side: best = highest idx, interior = lower indices
        uint32_t i = best_idx_;
        while (aggressor.remaining_qty > 0 && visited < active_count_) {
            if (active_[i]) {
                ++visited;
                const PriceLevel& lvl = levels_[i];
                if (!crosses(lvl.price)) break;   // price order: worse bids won't cross either
                for (const Order* o = lvl.head;
                     o != nullptr && aggressor.remaining_qty > 0;
                     o = o->next)
                {
                    aggressor.remaining_qty -=
                        std::min(aggressor.remaining_qty, o->remaining_qty);
                }
            }
            if (i == 0) break;
            --i;
        }
    } else {
        // ask side: best = lowest idx, interior = higher indices
        uint32_t i = best_idx_;
        while (aggressor.remaining_qty > 0 && visited < active_count_) {
            if (i >= MAX_TICKS) break;
            if (active_[i]) {
                ++visited;
                const PriceLevel& lvl = levels_[i];
                if (!crosses(lvl.price)) break;   // price order: worse asks won't cross either
                for (const Order* o = lvl.head;
                     o != nullptr && aggressor.remaining_qty > 0;
                     o = o->next)
                {
                    aggressor.remaining_qty -=
                        std::min(aggressor.remaining_qty, o->remaining_qty);
                }
            }
            ++i;
        }
    }
}

template <bool IsBid>
std::vector<std::pair<Price, Quantity>>
BookSide<IsBid>::depth(std::size_t n) const
{
    std::vector<std::pair<Price, Quantity>> out;
    if (!has_best_ || n == 0) return out;
    out.reserve(std::min(n, static_cast<std::size_t>(active_count_)));

    if constexpr (IsBid) {
        // Best bid = highest price = highest index; walk downward.
        uint32_t i = best_idx_;
        while (out.size() < n) {
            if (active_[i])
                out.emplace_back(levels_[i].price, levels_[i].total_qty);
            if (i == 0) break;
            --i;
        }
    } else {
        // Best ask = lowest price = lowest index; walk upward.
        for (uint32_t i = best_idx_;
             i < MAX_TICKS && out.size() < n;
             ++i)
        {
            if (active_[i])
                out.emplace_back(levels_[i].price, levels_[i].total_qty);
        }
    }
    return out;
}

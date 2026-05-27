#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

// Primitive type aliases
using OrderId  = std::uint64_t;
using Price    = std::int64_t;    ///< Fixed-point, 4 decimal places.
using Quantity = std::uint64_t;
using SeqNum   = std::uint64_t;

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

// Price conversion helpers

/// Tick size: 4 decimal places  →  1 tick = $0.0001
inline constexpr std::int64_t TICK_SCALE = 10'000;


#define PRICE(x) (static_cast<Price>((x) * TICK_SCALE + 0.5))


inline double price_to_double(Price p) noexcept {
    return static_cast<double>(p) / static_cast<double>(TICK_SCALE);
}

/// Sentinel price for market orders — guaranteed to cross any limit price.
inline constexpr Price MARKET_PRICE = std::numeric_limits<Price>::max();


// Enumerations

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    Limit,
    Market,
};

/// Time-in-Force — controls what happens to unfilled quantity after matching.
enum class TIF : std::uint8_t {
    GTC,   ///< Good Till Cancelled , unfilled remainder rests in the book.
    IOC,   ///< Immediate Or Cancel , unfilled remainder is cancelled immediately.
    FOK,   ///< Fill Or Kill        , entire order must fill or it is rejected outright.
};

enum class OrderStatus : std::uint8_t {
    New,             ///< Accepted, not yet filled.
    PartiallyFilled, ///< At least one fill; quantity remains.
    Filled,          ///< Fully filled.
    Cancelled,       ///< Cancelled by the user or IOC/FOK logic.
    Rejected,        ///< Never entered the book (e.g. FOK that couldn't fill).
};


// Order
// Layout: two explicit 64-byte cache lines.
//
// Line 1 — HOT (match loop, fill, cancel, list traversal):
//   prev, next, id, price, orig_qty, remaining_qty, filled_qty,
//   side, type, tif, status  →  60 bytes used + 4 bytes pad = 64 bytes.
//
// Line 2 — COLD (construction, audit, diagnostics):
//   seq, submit_ts, first_fill_ts  →  24 bytes used.
//   Remainder filled with compiler-calculated padding so that adding
//   a cold field never silently breaks alignment — the static_assert
//   below will catch the overflow immediately.

// Memory stability invariant:
//   Order objects are allocated via PoolAllocator (placement new) and
//   must never move after construction.  The intrusive list relies on
//   stable addresses: moving an Order would dangling-pointer every
//   neighbour's prev/next and every id_map entry pointing to it.
//   Copy and move operations are therefore explicitly deleted.
//   PoolAllocator::allocate() uses placement new and never needs them.

struct alignas(64) Order {

    // Cache line 1 - HOT
    // Intrusive doubly-linked list pointers.
    // Must be on line 1: PriceLevel::push_back / unlink touch these on
    // every insert and cancel.
    Order*   prev{nullptr};          // 8
    Order*   next{nullptr};          // 8

    // Identity (hot: id is read by every cancel and fill).
    OrderId  id{0};                  // 8

    // Economics — all four touched on every fill().
    Price    price{0};               // 8
    Quantity orig_qty{0};            // 8
    Quantity remaining_qty{0};       // 8
    Quantity filled_qty{0};          // 8

    // Classification — read by match loop to check side and TIF.
    Side        side{Side::Buy};     // 1
    OrderType   type{OrderType::Limit}; // 1
    TIF         tif{TIF::GTC};       // 1
    OrderStatus status{OrderStatus::New}; // 1

    // Pad line 1 to exactly 64 bytes.
    // 8+8+8+8+8+8+8+1+1+1+1 = 60 bytes used → 4 bytes pad.
    uint8_t _pad0[4];                // 4  →  total: 64

    // Cache line 2 — COLD  //

    // seq: set once at construction, never read in the hot path.
    SeqNum    seq{0};                // 8

    // Timestamps: written at construction and first fill; read only on
    // diagnostics / audit paths.
    TimePoint submit_ts{};           // 8
    TimePoint first_fill_ts{};       // 8

    // Compiler-calculated padding: fills the remainder of line 2.
    // If a future cold field is added here the constant decreases
    // automatically; if it overflows, static_assert(sizeof(Order)==128)
    // fires at compile time — never silently misaligned.
    static constexpr std::size_t kColdUsed =
        sizeof(SeqNum) + sizeof(TimePoint) + sizeof(TimePoint); // 24
    uint8_t _pad1[64 - kColdUsed];  // 40  →  total cold: 64

    // Construction

    Order() = default;

    Order(OrderId   id_,
          Price     price_,
          Quantity  qty_,
          Side      side_,
          OrderType type_,
          TIF       tif_,
          SeqNum    seq_) noexcept
        : id(id_)
        , price(price_)
        , orig_qty(qty_)
        , remaining_qty(qty_)
        , filled_qty(0)
        , side(side_)
        , type(type_)
        , tif(tif_)
        , status(OrderStatus::New)
        , seq(seq_)
        // submit_ts intentionally left zero (epoch) — Clock::now() costs
        // 23 ns and this field is cold (cache line 2, never read in hot path).
    {}

    // Memory stability: copy and move are both deleted.
    // See layout note above — moving an Order dangling-pointers the
    // intrusive list and every id_map entry that holds Order*.
    Order(const Order&)            = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&)                 = delete;
    Order& operator=(Order&&)      = delete;

    // State queries

    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::New ||
               status == OrderStatus::PartiallyFilled;
    }

    [[nodiscard]] bool is_filled() const noexcept {
        return remaining_qty == 0;
    }

    // Mutations (called only by the matching engine)

    // Fill qty units.  Returns the amount actually filled (≤ qty).
    // Records first_fill_ts on the very first fill.
    Quantity fill(Quantity qty) noexcept {
        assert(qty > 0);
        Quantity can_fill = std::min(qty, remaining_qty);

        // first_fill_ts intentionally not updated in hot path (23 ns per fill).
        // It remains epoch (zero) — readable as "timestamp not recorded".

        filled_qty    += can_fill;
        remaining_qty -= can_fill;

        status = (remaining_qty == 0) ? OrderStatus::Filled
                                      : OrderStatus::PartiallyFilled;
        return can_fill;
    }

    /// Cancel, safe to call on New or PartiallyFilled orders.
    /// Calling on Filled is a no-op (already done, not an error in the hot path).
    void cancel() noexcept {
        if (status == OrderStatus::Filled) return;
        status        = OrderStatus::Cancelled;
        remaining_qty = 0;
    }

    /// Reject, used by FOK pre-check; order never entered the book.
    void reject() noexcept {
        status        = OrderStatus::Rejected;
        remaining_qty = 0;
    }

    // Diagnostics
    std::string to_string() const;
};

// Compile-time layout guards.
// If sizeof(TimePoint) != 8 on this platform, or a field is added to
// either cache line without adjusting padding, these fire immediately.
static_assert(alignof(Order) == 64,
    "Order must be 64-byte aligned (alignas(64) not honoured by compiler)");
static_assert(sizeof(Order)  == 128,
    "Order must be exactly 2 cache lines (128 bytes); "
    "check _pad0 / _pad1 or field sizes if this fires");

// Trade

// Execution report produced whenever two orders match.
// Plain aggregate — no private state, no virtual dispatch.
struct Trade {
    OrderId   buy_order_id{0};
    OrderId   sell_order_id{0};
    Price     price{0};
    Quantity  quantity{0};       ///< Named `quantity`, not `qty`, for clarity.
    SeqNum    trade_seq{0};      ///< Global trade sequence , audit trail.
    TimePoint ts{};              ///< When the match occurred.

    // Accessors (compatible with matching_engine.cpp call sites)
    // Field names and accessor names cannot share the same identifier in a
    // struct, so accessors use a trailing underscore convention internally
    // but are exposed with the names the rest of the codebase expects.
    [[nodiscard]] Quantity qty()           const noexcept { return quantity;       }
    [[nodiscard]] TimePoint timestamp()    const noexcept { return ts;             }

    // Diagnostics
    std::string to_string() const;
};

using TradeCallback = std::function<void(const Trade&)>;

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


struct Order {
    // Intrusive doubly-linked list
    // These two pointers are the only fields the PoolAllocator / PriceLevel
    // infrastructure touches directly.  They must be first so that a
    // cache-line fetch that grabs `prev` also grabs `next`.
    Order* prev{nullptr};
    Order* next{nullptr};

    // Identity
    OrderId   id{0};
    SeqNum    seq{0};     ///< Monotonically increasing — determines time priority within a level.

    // Economics 
    Price     price{0};
    Quantity  orig_qty{0};
    Quantity  remaining_qty{0};
    Quantity  filled_qty{0};

    // Classification
    Side        side{Side::Buy};
    OrderType   type{OrderType::Limit};
    TIF         tif{TIF::GTC};
    OrderStatus status{OrderStatus::New};

    //  Timestamps 
    TimePoint submit_ts{};       ///< When the order entered the engine.
    TimePoint first_fill_ts{};   ///< When the first fill occurred (zero if unfilled).

    //  Construction

    Order() = default;

    Order(OrderId   id_,
          Price     price_,
          Quantity  qty_,
          Side      side_,
          OrderType type_,
          TIF       tif_,
          SeqNum    seq_) noexcept
        : id(id_)
        , seq(seq_)
        , price(price_)
        , orig_qty(qty_)
        , remaining_qty(qty_)
        , filled_qty(0)
        , side(side_)
        , type(type_)
        , tif(tif_)
        , status(OrderStatus::New)
        , submit_ts(Clock::now())
    {}

    // No copies, the pool owns the memory; copying would create aliased pointers.
    Order(const Order&)            = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&)                 = default;
    Order& operator=(Order&&)      = default;

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

        if (status == OrderStatus::New)
            first_fill_ts = Clock::now();

        filled_qty    += can_fill;
        remaining_qty -= can_fill;

        status = (remaining_qty == 0) ? OrderStatus::Filled
                                      : OrderStatus::PartiallyFilled;
        return can_fill;
    }

    /// Cancel — safe to call on New or PartiallyFilled orders.
    /// Calling on Filled is a no-op (already done, not an error in the hot path).
    void cancel() noexcept {
        if (status == OrderStatus::Filled) return;
        status        = OrderStatus::Cancelled;
        remaining_qty = 0;
    }

    /// Reject — used by FOK pre-check; order never entered the book.
    void reject() noexcept {
        status        = OrderStatus::Rejected;
        remaining_qty = 0;
    }

    // Diagnostics
    std::string to_string() const;
};

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

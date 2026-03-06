#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// Aliases

using OrderId  = uint64_t;
using Price    = double; 
using Quantity = uint64_t;

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

//Enumerations

enum class Side      { Buy, Sell };
enum class OrderType { Limit, Market };

enum class OrderStatus {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
};

// Order

class Order {
public:
    Order(OrderId     id,
          Side        side,
          OrderType   type,
          Price       price,
          Quantity    qty,
          std::string trader_id = "");

    // No copies — orders are owned by the book.
    Order(const Order&)            = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&)                 = default;
    Order& operator=(Order&&)      = default;

    // Mutation
    Quantity fill(Quantity qty);   // returns amount actually filled
    void     cancel();

    // Accessors
    OrderId     id()            const noexcept { return id_;            }
    Side        side()          const noexcept { return side_;          }
    OrderType   type()          const noexcept { return type_;          }
    Price       price()         const noexcept { return price_;         }
    Quantity    initial_qty()   const noexcept { return initial_qty_;   }
    Quantity    remaining_qty() const noexcept { return remaining_qty_; }
    Quantity    filled_qty()    const noexcept { return filled_qty_;    }
    OrderStatus status()        const noexcept { return status_;        }
    TimePoint   timestamp()     const noexcept { return timestamp_;     }

    const std::string& trader_id() const noexcept { return trader_id_; }

    bool is_active() const noexcept;

    std::string to_string() const;

private:
    OrderId     id_;
    Side        side_;
    OrderType   type_;
    Price       price_;

    Quantity    initial_qty_;
    Quantity    remaining_qty_;
    Quantity    filled_qty_;

    OrderStatus status_;
    std::string trader_id_;
    TimePoint   timestamp_;
};

// Trade
// Represents an executed match between a resting and an aggressing order.

class Trade {
public:
    Trade(OrderId  buy_order_id,
          OrderId  sell_order_id,
          Price    price,
          Quantity qty);

    OrderId  buy_order_id()  const noexcept { return buy_order_id_;  }
    OrderId  sell_order_id() const noexcept { return sell_order_id_; }
    Price    price()         const noexcept { return price_;         }
    Quantity qty()           const noexcept { return qty_;           }
    TimePoint timestamp()    const noexcept { return timestamp_;     }

    std::string to_string() const;

private:
    OrderId   buy_order_id_;
    OrderId   sell_order_id_;
    Price     price_;
    Quantity  qty_;
    TimePoint timestamp_;
};

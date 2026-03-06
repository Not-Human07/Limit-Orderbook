#include "order.hpp"

#include <stdexcept>
#include <sstream>

//Order

Order::Order(OrderId     id,
             Side        side,
             OrderType   type,
             Price       price,
             Quantity    qty,
             std::string trader_id)
    : id_(id)
    , side_(side)
    , type_(type)
    , price_(price)
    , initial_qty_(qty)
    , remaining_qty_(qty)
    , filled_qty_(0)
    , status_(OrderStatus::New)
    , trader_id_(std::move(trader_id))
    , timestamp_(Clock::now())
{
    if (qty == 0)
        throw std::invalid_argument("Order quantity must be > 0");

    // Market orders don't have a meaningful price; everything else must be positive.
    if (type_ != OrderType::Market && price_ <= 0)
        throw std::invalid_argument("Limit order price must be > 0");
}

// Fills the order with the given quantity, returning how much was filled.
Quantity Order::fill(Quantity qty)
{
    if (qty == 0)
        return 0;

    Quantity can_fill = std::min(qty, remaining_qty_);

    filled_qty_    += can_fill;
    remaining_qty_ -= can_fill;

    status_ = (remaining_qty_ == 0) ? OrderStatus::Filled
                                     : OrderStatus::PartiallyFilled;
    return can_fill;
}

void Order::cancel()
{
    if (status_ == OrderStatus::Filled)
        throw std::logic_error("Cannot cancel an already-filled order");

    status_        = OrderStatus::Cancelled;
    remaining_qty_ = 0;
}

bool Order::is_active() const noexcept
{
    return status_ == OrderStatus::New ||
           status_ == OrderStatus::PartiallyFilled;
}

std::string Order::to_string() const
{
    std::ostringstream oss;

    oss << "[Order " << id_
        << " | " << (side_ == Side::Buy ? "BUY" : "SELL")
        << " | " << (type_ == OrderType::Limit ? "LIMIT" : "MARKET")
        << " | price=" << price_
        << " | qty=" << initial_qty_
        << " | filled=" << filled_qty_
        << " | remaining=" << remaining_qty_
        << " | trader=" << trader_id_
        << " | status=";

    switch (status_) {
        case OrderStatus::New:              oss << "NEW";              break;
        case OrderStatus::PartiallyFilled:  oss << "PARTIAL";          break;
        case OrderStatus::Filled:           oss << "FILLED";           break;
        case OrderStatus::Cancelled:        oss << "CANCELLED";        break;
        default:                            oss << "UNKNOWN";          break;
    }

    oss << "]";
    return oss.str();
}

//Trade

Trade::Trade(OrderId  buy_order_id,
             OrderId  sell_order_id,
             Price    price,
             Quantity qty)
    : buy_order_id_(buy_order_id)
    , sell_order_id_(sell_order_id)
    , price_(price)
    , qty_(qty)
    , timestamp_(Clock::now())
{}

std::string Trade::to_string() const
{
    std::ostringstream oss;
    oss << "[Trade"
        << " buy="  << buy_order_id_
        << " sell=" << sell_order_id_
        << " price=" << price_
        << " qty="   << qty_
        << "]";
    return oss.str();
}

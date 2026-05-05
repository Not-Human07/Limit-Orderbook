#include "order.hpp"

#include <sstream>
#include <iomanip>

// Order::to_string
// For diagnostics and debug printing only — never called in the hot path.
// Uses price_to_double() for human-readable output while keeping all
// internal arithmetic in integer ticks.

std::string Order::to_string() const
{
    std::ostringstream oss;

    oss << "[Order " << id
        << " | seq="    << seq
        << " | "        << (side == Side::Buy ? "BUY" : "SELL")
        << " | "        << (type == OrderType::Limit ? "LIMIT" : "MARKET");

    // TIF
    oss << " | ";
    switch (tif) {
        case TIF::GTC: oss << "GTC"; break;
        case TIF::IOC: oss << "IOC"; break;
        case TIF::FOK: oss << "FOK"; break;
    }

    // Price — display as decimal, store as integer ticks.
    if (type == OrderType::Market)
        oss << " | price=MARKET";
    else
        oss << " | price=" << std::fixed << std::setprecision(4)
            << price_to_double(price);

    oss << " | orig_qty="      << orig_qty
        << " | filled="        << filled_qty
        << " | remaining="     << remaining_qty
        << " | status=";

    switch (status) {
        case OrderStatus::New:             oss << "NEW";     break;
        case OrderStatus::PartiallyFilled: oss << "PARTIAL"; break;
        case OrderStatus::Filled:          oss << "FILLED";  break;
        case OrderStatus::Cancelled:       oss << "CANCELLED"; break;
        case OrderStatus::Rejected:        oss << "REJECTED";  break;
    }

    oss << "]";
    return oss.str();
}

// Trade::to_string
std::string Trade::to_string() const
{
    std::ostringstream oss;
    oss << "[Trade"
        << " #"      << trade_seq
        << " buy="   << buy_order_id
        << " sell="  << sell_order_id
        << " price=" << std::fixed << std::setprecision(4)
                     << price_to_double(price)
        << " qty="   << quantity
        << "]";
    return oss.str();
}

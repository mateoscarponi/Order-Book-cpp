#pragma once

#include <cstddef>
#include <cstdint>

namespace ob {

// Prices are stored as integer ticks (for example, cents), never as floating
// point. 150.25 USD with a 0.01 tick is stored as 15025. Integer prices compare
// exactly, which matters when price levels are used as map keys.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // trade at the limit price or better; any remainder may rest
    Market,  // trade at any price; a remainder never rests
};

enum class TimeInForce : std::uint8_t {
    GTC,  // good till cancelled: the unfilled remainder rests on the book
    IOC,  // immediate or cancel: the unfilled remainder is cancelled
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;  // ignored for market orders
    Quantity quantity = 0;
};

struct Trade {
    OrderId buy_id = 0;
    OrderId sell_id = 0;
    Side aggressor = Side::Buy;  // side of the incoming order that caused the trade
    Price price = 0;             // always the resting order's price
    Quantity quantity = 0;

    friend bool operator==(const Trade& a, const Trade& b) {
        return a.buy_id == b.buy_id && a.sell_id == b.sell_id && a.aggressor == b.aggressor &&
               a.price == b.price && a.quantity == b.quantity;
    }
    friend bool operator!=(const Trade& a, const Trade& b) { return !(a == b); }
};

// Aggregated view of one price level, used for market-depth snapshots.
struct LevelView {
    Price price = 0;
    Quantity quantity = 0;
    std::size_t orders = 0;
};

// One resting order inside a price level, in queue order (front = next to trade).
struct OrderView {
    OrderId id = 0;
    Quantity remaining = 0;
};

inline Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

}  // namespace ob

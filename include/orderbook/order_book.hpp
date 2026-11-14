#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

enum class Reject : std::uint8_t {
    None,
    InvalidQuantity,  // quantity is zero
    InvalidPrice,     // limit price is zero or negative
    DuplicateId,      // an order with this id is already resting
    UnknownOrder,     // modify of an id that is not resting
};

struct SubmitResult {
    Reject reject = Reject::None;
    std::vector<Trade> trades;
    Quantity resting = 0;  // quantity left on the book after matching

    bool accepted() const { return reject == Reject::None; }
};

// A limit order book for a single instrument with price-time priority:
// the best price trades first, and at the same price the oldest order trades first.
//
// Complexity (L = number of price levels, F = number of fills):
//   submit  O(log L + F)
//   cancel  O(1) to find the order, O(log L) to drop an emptied level
//   best bid / best ask  O(1)
//
// Not thread-safe by design: each book has exactly one writer thread
// (see MatchingEngine), so the hot path needs no locks.
class OrderBook {
public:
    SubmitResult submit(Order order);
    bool cancel(OrderId id);

    // Cancel/replace. Reducing quantity at the same price keeps time priority.
    // Changing price or increasing quantity loses priority and may trade.
    SubmitResult modify(OrderId id, Price new_price, Quantity new_quantity);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::vector<LevelView> depth(Side side, std::size_t levels) const;
    Quantity volume_at(Side side, Price price) const;
    std::vector<OrderView> orders_at(Side side, Price price) const;  // queue order

    bool contains(OrderId id) const { return index_.count(id) != 0; }
    std::size_t order_count() const { return index_.size(); }
    std::size_t level_count(Side side) const {
        return side == Side::Buy ? bids_.size() : asks_.size();
    }

private:
    struct Resting {
        OrderId id;
        Quantity remaining;
    };
    // std::list keeps iterators valid when other orders are added or removed,
    // so the index below can point straight at an order for O(1) cancels.
    using Queue = std::list<Resting>;

    struct PriceLevel {
        Queue queue;         // FIFO: front is the oldest order
        Quantity total = 0;  // sum of remaining quantity at this price
    };

    // Bids sorted high to low and asks low to high, so begin() is always the best price.
    using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
    using Asks = std::map<Price, PriceLevel, std::less<Price>>;

    struct Locator {
        Side side;
        Price price;
        Queue::iterator position;
    };

    template <typename Book>
    void match(Order& incoming, Book& opposite, std::vector<Trade>& trades);

    template <typename Book>
    void add(Book& book, const Order& order);

    template <typename Book>
    void remove(Book& book, const Locator& locator);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace ob

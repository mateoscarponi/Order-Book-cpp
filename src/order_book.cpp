#include "orderbook/order_book.hpp"

#include <algorithm>
#include <iterator>

namespace ob {

namespace {

// True when an incoming limit order is willing to trade against a resting price.
bool crosses(Side incoming_side, Price limit, Price resting_price) {
    return incoming_side == Side::Buy ? limit >= resting_price : limit <= resting_price;
}

Trade make_trade(const Order& incoming, OrderId resting_id, Price price, Quantity quantity) {
    Trade trade;
    trade.aggressor = incoming.side;
    trade.buy_id = incoming.side == Side::Buy ? incoming.id : resting_id;
    trade.sell_id = incoming.side == Side::Buy ? resting_id : incoming.id;
    trade.price = price;
    trade.quantity = quantity;
    return trade;
}

}  // namespace

template <typename Book>
void OrderBook::match(Order& incoming, Book& opposite, std::vector<Trade>& trades) {
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();  // best opposite price
        const Price level_price = level_it->first;

        if (incoming.type == OrderType::Limit && !crosses(incoming.side, incoming.price, level_price)) {
            break;  // best opposite price is outside the limit: nothing else can trade
        }

        PriceLevel& level = level_it->second;
        while (incoming.quantity > 0 && !level.queue.empty()) {
            Resting& resting = level.queue.front();
            const Quantity fill = std::min(incoming.quantity, resting.remaining);

            trades.push_back(make_trade(incoming, resting.id, level_price, fill));
            incoming.quantity -= fill;
            resting.remaining -= fill;
            level.total -= fill;

            if (resting.remaining == 0) {
                index_.erase(resting.id);
                level.queue.pop_front();
            }
        }

        if (level.queue.empty()) {
            opposite.erase(level_it);
        }
    }
}

template <typename Book>
void OrderBook::add(Book& book, const Order& order) {
    PriceLevel& level = book[order.price];
    level.queue.push_back(Resting{order.id, order.quantity});
    level.total += order.quantity;
    index_.emplace(order.id, Locator{order.side, order.price, std::prev(level.queue.end())});
}

template <typename Book>
void OrderBook::remove(Book& book, const Locator& locator) {
    auto level_it = book.find(locator.price);
    PriceLevel& level = level_it->second;
    level.total -= locator.position->remaining;
    level.queue.erase(locator.position);
    if (level.queue.empty()) {
        book.erase(level_it);
    }
}

SubmitResult OrderBook::submit(Order order) {
    SubmitResult result;
    if (order.quantity == 0) {
        result.reject = Reject::InvalidQuantity;
        return result;
    }
    if (order.type == OrderType::Limit && order.price <= 0) {
        result.reject = Reject::InvalidPrice;
        return result;
    }
    if (contains(order.id)) {
        result.reject = Reject::DuplicateId;
        return result;
    }

    if (order.side == Side::Buy) {
        match(order, asks_, result.trades);
    } else {
        match(order, bids_, result.trades);
    }

    const bool can_rest = order.type == OrderType::Limit && order.tif == TimeInForce::GTC;
    if (order.quantity > 0 && can_rest) {
        if (order.side == Side::Buy) {
            add(bids_, order);
        } else {
            add(asks_, order);
        }
        result.resting = order.quantity;
    }
    return result;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    if (it->second.side == Side::Buy) {
        remove(bids_, it->second);
    } else {
        remove(asks_, it->second);
    }
    index_.erase(it);
    return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    SubmitResult result;
    auto it = index_.find(id);
    if (it == index_.end()) {
        result.reject = Reject::UnknownOrder;
        return result;
    }
    if (new_quantity == 0) {
        result.reject = Reject::InvalidQuantity;
        return result;
    }
    if (new_price <= 0) {
        result.reject = Reject::InvalidPrice;
        return result;
    }

    Locator& locator = it->second;
    Resting& resting = *locator.position;

    // Same price and smaller size: shrink in place and keep the place in the queue.
    if (new_price == locator.price && new_quantity <= resting.remaining) {
        const Quantity reduction = resting.remaining - new_quantity;
        resting.remaining = new_quantity;
        if (locator.side == Side::Buy) {
            bids_.find(locator.price)->second.total -= reduction;
        } else {
            asks_.find(locator.price)->second.total -= reduction;
        }
        result.resting = new_quantity;
        return result;
    }

    // Otherwise cancel and re-enter as a new order, which goes to the back of the queue.
    Order replacement;
    replacement.id = id;
    replacement.side = locator.side;
    replacement.type = OrderType::Limit;
    replacement.tif = TimeInForce::GTC;
    replacement.price = new_price;
    replacement.quantity = new_quantity;

    cancel(id);
    return submit(replacement);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<LevelView> out;
    auto collect = [&](const auto& book) {
        for (const auto& [price, level] : book) {
            if (out.size() == levels) break;
            out.push_back(LevelView{price, level.total, level.queue.size()});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

Quantity OrderBook::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

std::vector<OrderView> OrderBook::orders_at(Side side, Price price) const {
    std::vector<OrderView> out;
    auto collect = [&](const auto& book) {
        auto it = book.find(price);
        if (it == book.end()) return;
        for (const Resting& resting : it->second.queue) out.push_back(OrderView{resting.id, resting.remaining});
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

}  // namespace ob

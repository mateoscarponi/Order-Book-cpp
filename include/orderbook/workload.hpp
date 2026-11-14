#pragma once

#include <random>
#include <string>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

// A synthetic order flow, used by the benchmark and the multithreading test.
struct WorkItem {
    enum class Kind : std::uint8_t { Submit, Cancel };
    Kind kind = Kind::Submit;
    std::size_t symbol = 0;  // index into symbol_name()
    Order order;
};

inline std::string symbol_name(std::size_t index) { return "SYM" + std::to_string(index); }

// Deterministic for a given seed. Prices cluster around 100.00 (10000 ticks) so
// that buys and sells cross often. Mix: ~85% limit, ~5% market, ~10% cancels.
inline std::vector<WorkItem> generate_workload(std::size_t count, std::size_t symbols,
                                               std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> pick_symbol(0, symbols - 1);
    std::uniform_int_distribution<int> pick_action(0, 99);
    std::uniform_int_distribution<Price> pick_offset(-50, 50);
    std::uniform_int_distribution<Quantity> pick_quantity(1, 100);

    std::vector<WorkItem> items;
    items.reserve(count);
    std::vector<std::vector<OrderId>> ids_by_symbol(symbols);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < count; ++i) {
        WorkItem item;
        item.symbol = pick_symbol(rng);
        const int action = pick_action(rng);
        auto& ids = ids_by_symbol[item.symbol];

        if (action < 10 && !ids.empty()) {
            std::uniform_int_distribution<std::size_t> pick_id(0, ids.size() - 1);
            item.kind = WorkItem::Kind::Cancel;
            item.order.id = ids[pick_id(rng)];
        } else {
            Order& order = item.order;
            order.id = next_id++;
            order.side = (action % 2 == 0) ? Side::Buy : Side::Sell;
            order.type = action < 15 ? OrderType::Market : OrderType::Limit;
            order.tif = action >= 95 ? TimeInForce::IOC : TimeInForce::GTC;
            // Buyers lean slightly below mid and sellers slightly above, so the book has depth.
            const Price lean = order.side == Side::Buy ? -5 : 5;
            order.price = 10000 + lean + pick_offset(rng);
            order.quantity = pick_quantity(rng);
            ids.push_back(order.id);
        }
        items.push_back(item);
    }
    return items;
}

}  // namespace ob

// Unit tests with a tiny built-in runner, so the project builds with no dependencies.

#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "orderbook/matching_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/workload.hpp"

namespace {

int g_failures = 0;

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                  \
    void name();                                    \
    const Registrar name##_registrar(#name, &name); \
    void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << "  CHECK(" #cond ")\n"; \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

using namespace ob;

Order limit(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif = TimeInForce::GTC) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = tif;
    o.price = price;
    o.quantity = qty;
    return o;
}

Order market(OrderId id, Side side, Quantity qty) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::Market;
    o.quantity = qty;
    return o;
}

// ---------------------------------------------------------------- order book

TEST(resting_order_does_not_trade) {
    OrderBook book;
    auto r = book.submit(limit(1, Side::Buy, 100, 10));
    CHECK(r.accepted());
    CHECK(r.trades.empty());
    CHECK_EQ(r.resting, 10u);
    CHECK_EQ(book.best_bid(), std::optional<Price>(100));
    CHECK(!book.best_ask().has_value());
}

TEST(trade_happens_at_resting_price) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 101, 50));
    auto r = book.submit(limit(2, Side::Buy, 105, 50));
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 101);  // buyer gets price improvement
    CHECK_EQ(r.trades[0].buy_id, 2u);
    CHECK_EQ(r.trades[0].sell_id, 1u);
    CHECK(r.trades[0].aggressor == Side::Buy);
    CHECK_EQ(book.order_count(), 0u);
}

TEST(better_price_trades_first) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 102, 10));
    book.submit(limit(2, Side::Sell, 101, 10));
    auto r = book.submit(limit(3, Side::Buy, 102, 10));
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].sell_id, 2u);
    CHECK_EQ(book.best_ask(), std::optional<Price>(102));
}

TEST(older_order_trades_first_at_same_price) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 101, 10));
    book.submit(limit(2, Side::Sell, 101, 10));
    auto r = book.submit(limit(3, Side::Buy, 101, 15));
    CHECK_EQ(r.trades.size(), 2u);
    CHECK_EQ(r.trades[0].sell_id, 1u);
    CHECK_EQ(r.trades[0].quantity, 10u);
    CHECK_EQ(r.trades[1].sell_id, 2u);
    CHECK_EQ(r.trades[1].quantity, 5u);
    CHECK_EQ(book.volume_at(Side::Sell, 101), 5u);
}

TEST(partial_fill_rests_the_remainder) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 30));
    auto r = book.submit(limit(2, Side::Buy, 100, 100));
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 30u);
    CHECK_EQ(r.resting, 70u);
    CHECK_EQ(book.best_bid(), std::optional<Price>(100));
    CHECK_EQ(book.volume_at(Side::Buy, 100), 70u);
    CHECK(!book.best_ask().has_value());
}

TEST(order_sweeps_several_levels_until_limit) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 10));
    book.submit(limit(2, Side::Sell, 101, 10));
    book.submit(limit(3, Side::Sell, 102, 10));
    book.submit(limit(4, Side::Sell, 103, 10));
    auto r = book.submit(limit(5, Side::Buy, 102, 100));
    CHECK_EQ(r.trades.size(), 3u);
    CHECK_EQ(r.trades[2].price, 102);
    CHECK_EQ(r.resting, 70u);
    CHECK_EQ(book.best_bid(), std::optional<Price>(102));
    CHECK_EQ(book.best_ask(), std::optional<Price>(103));
}

TEST(market_order_never_rests) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 100, 40));
    auto r = book.submit(market(2, Side::Buy, 100));
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 40u);
    CHECK_EQ(r.resting, 0u);
    CHECK_EQ(book.order_count(), 0u);

    auto empty = book.submit(market(3, Side::Sell, 10));
    CHECK(empty.accepted());
    CHECK(empty.trades.empty());
    CHECK_EQ(book.order_count(), 0u);
}

TEST(ioc_remainder_is_cancelled) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 25));
    auto r = book.submit(limit(2, Side::Sell, 100, 60, TimeInForce::IOC));
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 25u);
    CHECK_EQ(r.resting, 0u);
    CHECK(!book.contains(2));
    CHECK_EQ(book.order_count(), 0u);
}

TEST(cancel_removes_order_and_empty_level) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Buy, 99, 10));
    CHECK(book.cancel(1));
    CHECK(!book.cancel(1));   // already gone
    CHECK(!book.cancel(42));  // never existed
    CHECK_EQ(book.best_bid(), std::optional<Price>(99));
    CHECK_EQ(book.level_count(Side::Buy), 1u);
}

TEST(modify_down_keeps_time_priority) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 101, 10));
    book.submit(limit(2, Side::Sell, 101, 10));
    auto m = book.modify(1, 101, 4);
    CHECK(m.accepted());
    CHECK_EQ(book.volume_at(Side::Sell, 101), 14u);
    auto r = book.submit(limit(3, Side::Buy, 101, 4));
    CHECK_EQ(r.trades[0].sell_id, 1u);  // still first in line
}

TEST(orders_at_lists_queue_in_time_order) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 101, 10));
    book.submit(limit(2, Side::Sell, 101, 20));
    book.submit(limit(3, Side::Sell, 101, 30));
    book.submit(limit(4, Side::Buy, 101, 15));  // fills #1 and 5 of #2
    const auto queue = book.orders_at(Side::Sell, 101);
    CHECK_EQ(queue.size(), 2u);
    CHECK_EQ(queue[0].id, 2u);
    CHECK_EQ(queue[0].remaining, 15u);
    CHECK_EQ(queue[1].id, 3u);
    CHECK(book.orders_at(Side::Buy, 101).empty());
}

TEST(modify_up_loses_time_priority) {
    OrderBook book;
    book.submit(limit(1, Side::Sell, 101, 10));
    book.submit(limit(2, Side::Sell, 101, 10));
    book.modify(1, 101, 20);
    auto r = book.submit(limit(3, Side::Buy, 101, 5));
    CHECK_EQ(r.trades[0].sell_id, 2u);  // order 1 went to the back
}

TEST(modify_to_crossing_price_trades) {
    OrderBook book;
    book.submit(limit(1, Side::Buy, 100, 10));
    book.submit(limit(2, Side::Sell, 105, 10));
    auto r = book.modify(1, 105, 10);
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 105);
    CHECK_EQ(book.order_count(), 0u);
}

TEST(invalid_orders_are_rejected) {
    OrderBook book;
    CHECK(book.submit(limit(1, Side::Buy, 100, 0)).reject == Reject::InvalidQuantity);
    CHECK(book.submit(limit(2, Side::Buy, 0, 10)).reject == Reject::InvalidPrice);
    CHECK(book.submit(limit(3, Side::Buy, -5, 10)).reject == Reject::InvalidPrice);
    book.submit(limit(4, Side::Buy, 100, 10));
    CHECK(book.submit(limit(4, Side::Buy, 100, 10)).reject == Reject::DuplicateId);
    CHECK(book.modify(99, 100, 10).reject == Reject::UnknownOrder);
    CHECK_EQ(book.order_count(), 1u);
}

TEST(random_flow_never_leaves_book_crossed) {
    OrderBook book;
    const auto items = generate_workload(50000, 1, 7);
    Quantity traded = 0;
    for (const auto& item : items) {
        if (item.kind == WorkItem::Kind::Cancel) {
            book.cancel(item.order.id);
        } else {
            for (const auto& t : book.submit(item.order).trades) traded += t.quantity;
        }
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (bid && ask) CHECK(*bid < *ask);
    }
    CHECK(traded > 0);

    // Level totals must equal what the depth snapshot reports, side by side.
    std::size_t orders_in_depth = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
        for (const auto& level : book.depth(side, 1000000)) {
            CHECK(level.quantity > 0);
            CHECK_EQ(level.quantity, book.volume_at(side, level.price));
            orders_in_depth += level.orders;
        }
    }
    CHECK_EQ(orders_in_depth, book.order_count());
}

// ---------------------------------------------------------------- matching engine

// The multithreaded engine must produce exactly the same trades, symbol by symbol,
// as a plain single-threaded replay of the same order flow.
TEST(engine_matches_single_threaded_replay) {
    const std::size_t symbols = 16;
    const auto items = generate_workload(200000, symbols, 2026);

    std::map<std::string, std::vector<Trade>> expected;
    {
        std::map<std::string, OrderBook> books;
        for (const auto& item : items) {
            const std::string symbol = symbol_name(item.symbol);
            OrderBook& book = books[symbol];
            if (item.kind == WorkItem::Kind::Cancel) {
                book.cancel(item.order.id);
            } else {
                for (const auto& t : book.submit(item.order).trades) expected[symbol].push_back(t);
            }
        }
    }

    std::mutex mutex;
    std::map<std::string, std::vector<Trade>> actual;
    MatchingEngine engine(4, [&](const std::string& symbol, const Trade& trade) {
        std::lock_guard<std::mutex> lock(mutex);
        actual[symbol].push_back(trade);
    });
    for (const auto& item : items) {
        const std::string symbol = symbol_name(item.symbol);
        if (item.kind == WorkItem::Kind::Cancel) {
            CHECK(engine.cancel(symbol, item.order.id));
        } else {
            CHECK(engine.submit(symbol, item.order));
        }
    }
    engine.stop();

    CHECK_EQ(engine.processed(), items.size());
    CHECK_EQ(actual.size(), expected.size());
    CHECK(actual == expected);
    CHECK(engine.book("SYM0") != nullptr);
    CHECK(!engine.submit("SYM0", limit(999999999, Side::Buy, 100, 1)));  // stopped
}

TEST(engine_batched_submission_matches_replay) {
    const auto items = generate_workload(100000, 8, 99);
    std::map<std::string, OrderBook> books;
    std::uint64_t expected = 0;
    for (const auto& item : items) {
        OrderBook& book = books[symbol_name(item.symbol)];
        if (item.kind == WorkItem::Kind::Cancel) {
            book.cancel(item.order.id);
        } else {
            expected += book.submit(item.order).trades.size();
        }
    }

    using Request = MatchingEngine::Request;
    std::atomic<std::uint64_t> trades{0};
    MatchingEngine engine(3, [&](const std::string&, const Trade&) { trades.fetch_add(1); });
    std::vector<Request> batch;
    for (const auto& item : items) {
        const auto kind = item.kind == WorkItem::Kind::Cancel ? Request::Kind::Cancel : Request::Kind::Submit;
        batch.push_back(Request{kind, symbol_name(item.symbol), item.order});
        if (batch.size() == 500) CHECK(engine.submit_batch(batch));
    }
    CHECK(engine.submit_batch(batch));
    engine.stop();

    CHECK_EQ(trades.load(), expected);
    for (const auto& [symbol, book] : books) {
        const OrderBook* engine_book = engine.book(symbol);
        CHECK(engine_book != nullptr);
        if (!engine_book) continue;
        CHECK_EQ(engine_book->order_count(), book.order_count());
        CHECK(engine_book->best_bid() == book.best_bid());
        CHECK(engine_book->best_ask() == book.best_ask());
    }
}

TEST(engine_accepts_orders_from_many_producer_threads) {
    std::atomic<std::uint64_t> trades{0};
    MatchingEngine engine(3, [&](const std::string&, const Trade&) { trades.fetch_add(1); });

    // Four producers, each with its own symbol, all crossing against themselves.
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&engine, p] {
            const std::string symbol = "P" + std::to_string(p);
            const OrderId base = static_cast<OrderId>(p) * 1000000;
            for (OrderId i = 0; i < 5000; ++i) {
                engine.submit(symbol, limit(base + 2 * i + 1, Side::Sell, 100, 1));
                engine.submit(symbol, limit(base + 2 * i + 2, Side::Buy, 100, 1));
            }
        });
    }
    for (auto& t : producers) t.join();
    engine.stop();

    CHECK_EQ(trades.load(), 4u * 5000u);
    for (int p = 0; p < 4; ++p) {
        const OrderBook* book = engine.book("P" + std::to_string(p));
        CHECK(book != nullptr);
        if (book) CHECK_EQ(book->order_count(), 0u);
    }
}

}  // namespace

int main() {
    int failed_tests = 0;
    for (const auto& test : registry()) {
        const int before = g_failures;
        test.fn();
        const bool ok = g_failures == before;
        if (!ok) ++failed_tests;
        std::cout << (ok ? "[ OK ] " : "[FAIL] ") << test.name << "\n";
    }
    std::cout << "\n" << registry().size() - static_cast<std::size_t>(failed_tests) << "/"
              << registry().size() << " tests passed\n";
    return failed_tests == 0 ? 0 : 1;
}

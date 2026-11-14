#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "orderbook/blocking_queue.hpp"
#include "orderbook/order_book.hpp"

namespace ob {

// Runs many order books in parallel.
//
// Each symbol is assigned to one worker thread (hash of the symbol), and only that
// thread ever touches the symbol's book. This is the "single writer" design: books
// need no locks, orders for one symbol are processed strictly in arrival order, and
// different symbols scale across cores. The only shared state is each worker's queue.
class MatchingEngine {
public:
    struct Request {
        enum class Kind : std::uint8_t { Submit, Cancel };
        Kind kind = Kind::Submit;
        std::string symbol;
        Order order;  // for Cancel only order.id is used
    };

    // Called on a worker thread for every trade, so it must be thread-safe.
    using TradeHandler = std::function<void(const std::string& symbol, const Trade& trade)>;

    MatchingEngine(std::size_t worker_count, TradeHandler on_trade);
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Thread-safe. Return false after stop().
    bool submit(const std::string& symbol, const Order& order);
    bool cancel(const std::string& symbol, OrderId id);

    // Thread-safe. Hands over many requests with one lock and one wake-up per worker,
    // instead of one per request. Order within each symbol is preserved. Clears `requests`.
    bool submit_batch(std::vector<Request>& requests);

    // Processes everything already queued, then joins the workers. Idempotent.
    void stop();

    // Read access to a book. Only valid after stop(), when no worker is running.
    const OrderBook* book(const std::string& symbol) const;

    std::size_t worker_count() const { return workers_.size(); }
    std::uint64_t processed() const { return processed_.load(std::memory_order_relaxed); }

private:
    struct Worker {
        BlockingQueue<Request> queue;
        std::unordered_map<std::string, OrderBook> books;
        std::thread thread;
    };

    std::size_t shard(const std::string& symbol) const;
    void run(Worker& worker);

    std::vector<std::unique_ptr<Worker>> workers_;
    TradeHandler on_trade_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> processed_{0};
};

}  // namespace ob

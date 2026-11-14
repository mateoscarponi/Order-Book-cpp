#include "orderbook/matching_engine.hpp"

#include <deque>
#include <stdexcept>

namespace ob {

MatchingEngine::MatchingEngine(std::size_t worker_count, TradeHandler on_trade)
    : on_trade_(std::move(on_trade)) {
    if (worker_count == 0) {
        throw std::invalid_argument("MatchingEngine needs at least one worker");
    }
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.push_back(std::make_unique<Worker>());
    }
    // Start threads only after every worker exists, so no thread sees a half-built vector.
    for (auto& worker : workers_) {
        Worker* w = worker.get();
        w->thread = std::thread([this, w] { run(*w); });
    }
}

MatchingEngine::~MatchingEngine() { stop(); }

std::size_t MatchingEngine::shard(const std::string& symbol) const {
    return std::hash<std::string>{}(symbol) % workers_.size();
}

bool MatchingEngine::submit(const std::string& symbol, const Order& order) {
    if (stopped_.load(std::memory_order_acquire)) return false;
    return workers_[shard(symbol)]->queue.push(Request{Request::Kind::Submit, symbol, order});
}

bool MatchingEngine::cancel(const std::string& symbol, OrderId id) {
    if (stopped_.load(std::memory_order_acquire)) return false;
    Order order;
    order.id = id;
    return workers_[shard(symbol)]->queue.push(Request{Request::Kind::Cancel, symbol, order});
}

bool MatchingEngine::submit_batch(std::vector<Request>& requests) {
    if (stopped_.load(std::memory_order_acquire)) return false;
    std::vector<std::vector<Request>> per_worker(workers_.size());
    for (Request& request : requests) {
        per_worker[shard(request.symbol)].push_back(std::move(request));
    }
    requests.clear();
    bool ok = true;
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        ok = workers_[i]->queue.push_batch(per_worker[i]) && ok;
    }
    return ok;
}

void MatchingEngine::run(Worker& worker) {
    std::deque<Request> batch;
    // Drain the queue in batches: one lock per batch instead of one per order.
    while (worker.queue.pop_all(batch)) {
        for (Request& request : batch) {
            OrderBook& book = worker.books[request.symbol];
            if (request.kind == Request::Kind::Submit) {
                const SubmitResult result = book.submit(request.order);
                if (on_trade_) {
                    for (const Trade& trade : result.trades) on_trade_(request.symbol, trade);
                }
            } else {
                book.cancel(request.order.id);
            }
        }
        processed_.fetch_add(batch.size(), std::memory_order_relaxed);
        batch.clear();
    }
}

void MatchingEngine::stop() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return;
    for (auto& worker : workers_) worker->queue.close();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
}

const OrderBook* MatchingEngine::book(const std::string& symbol) const {
    if (!stopped_.load(std::memory_order_acquire)) return nullptr;
    const Worker& worker = *workers_[shard(symbol)];
    auto it = worker.books.find(symbol);
    return it == worker.books.end() ? nullptr : &it->second;
}

}  // namespace ob

// Throughput benchmark: the same synthetic order flow is run through
//   1) plain order books on one thread,
//   2) the MatchingEngine with N worker threads, orders handed over one by one, and
//   3) the same engine with orders handed over in batches of 1024,
// and all trade counts must match.
//
// usage: bench [orders=2000000] [symbols=16] [workers=4]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "orderbook/matching_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/workload.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;
using Request = MatchingEngine::Request;

namespace {

struct RunResult {
    double seconds = 0;
    std::uint64_t trades = 0;
};

RunResult run_single_thread(const std::vector<WorkItem>& items, std::size_t symbols) {
    RunResult result;
    const auto start = Clock::now();
    std::vector<OrderBook> books(symbols);
    for (const auto& item : items) {
        OrderBook& book = books[item.symbol];
        if (item.kind == WorkItem::Kind::Cancel) {
            book.cancel(item.order.id);
        } else {
            result.trades += book.submit(item.order).trades.size();
        }
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

// Timing covers handing every order to the engine and draining all queues.
RunResult run_engine(const std::vector<WorkItem>& items, const std::vector<std::string>& names,
                     std::size_t workers, std::size_t batch_size) {
    std::atomic<std::uint64_t> trades{0};
    const auto start = Clock::now();
    {
        MatchingEngine engine(workers, [&](const std::string&, const Trade&) {
            trades.fetch_add(1, std::memory_order_relaxed);
        });
        std::vector<Request> batch;
        batch.reserve(batch_size);
        for (const auto& item : items) {
            const bool is_cancel = item.kind == WorkItem::Kind::Cancel;
            if (batch_size <= 1) {
                if (is_cancel) {
                    engine.cancel(names[item.symbol], item.order.id);
                } else {
                    engine.submit(names[item.symbol], item.order);
                }
                continue;
            }
            batch.push_back(Request{is_cancel ? Request::Kind::Cancel : Request::Kind::Submit,
                                    names[item.symbol], item.order});
            if (batch.size() == batch_size) engine.submit_batch(batch);
        }
        engine.submit_batch(batch);
        engine.stop();
    }
    RunResult result;
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    result.trades = trades.load();
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t orders = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
    const std::size_t symbols = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 16;
    const std::size_t workers = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 4;
    if (orders == 0 || symbols == 0 || workers == 0) {
        std::fprintf(stderr, "orders, symbols and workers must be positive\n");
        return 2;
    }

    std::printf("Generating %zu commands across %zu symbols...\n", orders, symbols);
    const auto items = generate_workload(orders, symbols, 42);
    std::vector<std::string> names;
    for (std::size_t s = 0; s < symbols; ++s) names.push_back(symbol_name(s));

    const RunResult single = run_single_thread(items, symbols);
    const RunResult one_by_one = run_engine(items, names, workers, 1);
    const RunResult batched = run_engine(items, names, workers, 1024);

    auto row = [&](const char* label, const RunResult& r) {
        std::printf("%-30s %9.3f %14.0f %10llu\n", label, r.seconds, static_cast<double>(orders) / r.seconds,
                    static_cast<unsigned long long>(r.trades));
    };
    std::printf("\n%-30s %9s %14s %10s\n", "mode", "seconds", "orders/sec", "trades");
    row("single thread (books only)", single);
    char label[64];
    std::snprintf(label, sizeof label, "engine, %zu workers", workers);
    row(label, one_by_one);
    std::snprintf(label, sizeof label, "engine, %zu workers, batched", workers);
    row(label, batched);

    const bool same = single.trades == one_by_one.trades && single.trades == batched.trades;
    std::printf("\ntrade counts %s\n", same ? "match" : "DO NOT MATCH");
    return same ? 0 : 1;
}

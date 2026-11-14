#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace ob {

// Multi-producer, single-consumer FIFO queue guarded by a mutex and a condition variable.
// The consumer blocks until an item arrives or the queue is closed.
//
// Producers only signal when the queue goes from empty to non-empty: if items are
// already waiting, the consumer is awake (or about to drain them), so another
// notify would just be a wasted system call. This relies on a single consumer.
template <typename T>
class BlockingQueue {
public:
    // Returns false if the queue was already closed.
    bool push(T item) {
        bool was_empty;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            was_empty = items_.empty();
            items_.push_back(std::move(item));
        }
        if (was_empty) ready_.notify_one();
        return true;
    }

    // Pushes a whole batch under one lock and with at most one wake-up.
    bool push_batch(std::vector<T>& batch) {
        if (batch.empty()) return true;
        bool was_empty;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            was_empty = items_.empty();
            for (auto& item : batch) items_.push_back(std::move(item));
        }
        batch.clear();
        if (was_empty) ready_.notify_one();
        return true;
    }

    // Blocks until an item is available. Returns nullopt once closed and drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Moves everything currently queued into `out`. Blocks while empty.
    // Returns false once closed and drained.
    bool pop_all(std::deque<T>& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        out.swap(items_);
        return true;
    }

    // Wakes all consumers. Items already queued are still delivered.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace ob

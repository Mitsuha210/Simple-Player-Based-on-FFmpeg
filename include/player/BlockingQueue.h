//阻塞队列->用于不同线程之间传输数据
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>

namespace sim_player {

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t max_size = 128) : max_size_(max_size) {} //防止隐式转换

    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        //一直wait直到aborted或队列不满(因为要push)
        not_full_.wait(lock, [this] { return aborted_ || queue_.size() < max_size_; });
        if (aborted_) {
            return false;
        }
        queue_.push(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        //一直wait直到aborted或队列不空(因为要pop)
        not_empty_.wait(lock, [this] { return aborted_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return value;
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        //唤醒所有可能在等待的线程
        aborted_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
        not_full_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::size_t max_size_;
    bool aborted_ = false;
    mutable std::mutex mutex_; //mutable 的作用是：即使在 const 函数里（比如 empty()），也允许加锁。
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
};

}  // namespace sim_player

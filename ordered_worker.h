#ifndef UDP2RAW_ORDERED_WORKER_H
#define UDP2RAW_ORDERED_WORKER_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

// 单一消费者按提交顺序取回；容量包含排队、执行中和等待取回的全部任务。
template <class T>
class ordered_worker_pool {
    struct slot {
        T value;
        bool ready = false;
    };
    std::vector<slot> slots_;
    std::vector<std::thread> threads_;
    std::function<void(T &)> process_;
    std::function<void()> notify_;
    std::mutex mutex_;
    std::condition_variable available_;
    uint64_t submitted_ = 0, claimed_ = 0, consumed_ = 0;
    bool stopping_ = false;

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            available_.wait(lock, [this] { return stopping_ || claimed_ != submitted_; });
            if (claimed_ == submitted_) return;
            slot &item = slots_[claimed_++ % slots_.size()];
            lock.unlock();
            process_(item.value);
            lock.lock();
            item.ready = true;
            lock.unlock();
            notify_();
        }
    }

public:
    ordered_worker_pool(size_t workers, size_t capacity,
                        std::function<void(T &)> process, std::function<void()> notify)
        : slots_(capacity), process_(std::move(process)), notify_(std::move(notify)) {
        if (workers == 0 || capacity == 0) throw std::invalid_argument("empty worker pool");
        threads_.reserve(workers);
        try {
            for (size_t i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
        } catch (const std::system_error &) {
            stop();
            throw;
        }
    }
    ~ordered_worker_pool() { stop(); }
    ordered_worker_pool(const ordered_worker_pool &) = delete;
    ordered_worker_pool &operator=(const ordered_worker_pool &) = delete;

    bool try_submit(const T &value) {
        return try_submit_with([&value](T &target) { target = value; });
    }
    template <class Fill>
    bool try_submit_with(Fill fill) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_ || submitted_ - consumed_ == slots_.size()) return false;
        slot &item = slots_[submitted_ % slots_.size()];
        fill(item.value);
        item.ready = false;
        ++submitted_;
        lock.unlock();
        available_.notify_one();
        return true;
    }
    bool try_pop(T &value) {
        return try_consume([&value](T &source) { value = std::move(source); });
    }
    template <class Consume>
    bool try_consume(Consume consume) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (consumed_ == submitted_) return false;
        slot &item = slots_[consumed_ % slots_.size()];
        if (!item.ready) return false;
        // consumed 在回调完成后才递增，因此生产者不会重用该槽位；网络发送不占用队列锁。
        lock.unlock();
        consume(item.value);
        lock.lock();
        ++consumed_;
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        available_.notify_all();
        for (auto &thread : threads_) thread.join();
        threads_.clear();
    }
};
#endif

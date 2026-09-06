// Minimal fork/join pool. The solver runs many short parallel rounds, so the
// worker threads are created once and parked on a condition variable between
// rounds instead of being respawned.
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tcs {

class ThreadPool {
public:
    explicit ThreadPool(int threads) : count_(threads > 1 ? threads : 1) {
        if (count_ <= 1) return;
        workers_.reserve(static_cast<size_t>(count_) - 1);
        for (int i = 1; i < count_; ++i) {
            workers_.emplace_back([this, i] { loop(i); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            ++generation_;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    int size() const { return count_; }

    // Runs fn(0..size()-1), the caller taking slot 0, and returns once all
    // slots have finished.
    void run(const std::function<void(int)>& fn) {
        if (count_ == 1) {
            fn(0);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            job_ = &fn;
            pending_ = count_ - 1;
            ++generation_;
        }
        cv_.notify_all();
        fn(0);
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [this] { return pending_ == 0; });
        job_ = nullptr;
    }

private:
    void loop(int index) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this, &seen] { return stop_ || generation_ != seen; });
                seen = generation_;
                if (stop_) return;
                job = job_;
            }
            if (job) (*job)(index);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (--pending_ == 0) done_.notify_one();
            }
        }
    }

    int count_;
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable done_;
    const std::function<void(int)>* job_ = nullptr;
    uint64_t generation_ = 0;
    int pending_ = 0;
    bool stop_ = false;
};

}  // namespace tcs

#pragma once
/* One persistent worker thread with a small FIFO queue, for work that must never run on (or block) the UI frame thread: the Windows toast
 * calls (WinRT apartment init + Show take hundreds of ms). Header-only so a console test can exercise it without WinRT.
 * - The thread starts on the first post() (so a process that never toasts never gets one) and runs `on_start` / `on_exit` on itself
 *   (the toast worker initialises / uninitialises its WinRT apartment there).
 * - post() never blocks: it queues and returns true, or returns false when the queue is full or the worker was stopped.
 * - stop() lets the queued jobs finish, then joins. Safe to call twice; the destructor calls it. */
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace hubtoast {

class SerialWorker {
public:
    explicit SerialWorker(size_t cap = 16, std::function<void()> on_start = nullptr, std::function<void()> on_exit = nullptr)
        : cap_(cap), on_start_(std::move(on_start)), on_exit_(std::move(on_exit)) {}
    ~SerialWorker() { stop(); }
    SerialWorker(const SerialWorker&) = delete;
    SerialWorker& operator=(const SerialWorker&) = delete;

    bool post(std::function<void()> job) {
        std::lock_guard<std::mutex> l(mu_);
        if (stopped_ || q_.size() >= cap_) return false;
        q_.push_back(std::move(job));
        if (!th_.joinable()) th_ = std::thread([this] { run(); });
        cv_.notify_one();
        return true;
    }

    void stop() {
        std::thread t;
        {
            std::lock_guard<std::mutex> l(mu_);
            stopped_ = true;
            t = std::move(th_);
            cv_.notify_all();
        }
        if (t.joinable()) t.join();
    }

    size_t pending() const { std::lock_guard<std::mutex> l(mu_); return q_.size(); }

private:
    void run() {
        if (on_start_) on_start_();
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return stopped_ || !q_.empty(); });
                if (q_.empty()) break;   // stopped and drained
                job = std::move(q_.front());
                q_.pop_front();
            }
            if (job) job();
        }
        if (on_exit_) on_exit_();
    }

    const size_t cap_;
    std::function<void()> on_start_, on_exit_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::thread th_;
    bool stopped_ = false;
};

}  // namespace hubtoast

// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: a small pool of worker threads. Newest tasks run first so items
// that just scrolled into view get their previews before older requests.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace beluga {

class WorkQueue {
public:
    explicit WorkQueue(int threads, bool lifo = true) : lifo_(lifo)
    {
        for (int i = 0; i < threads; ++i)
            std::thread([this] { loop(); }).detach();
    }
    void push(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> l(m_);
            tasks_.push_back(std::move(task));
            while (tasks_.size() > 20000) tasks_.pop_front();
        }
        cv_.notify_one();
    }
    void clear()
    {
        std::lock_guard<std::mutex> l(m_);
        tasks_.clear();
    }

private:
    void loop()
    {
        for (;;) {
            std::function<void()> t;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return !tasks_.empty(); });
                if (lifo_) {
                    t = std::move(tasks_.back());
                    tasks_.pop_back();
                } else {
                    t = std::move(tasks_.front());
                    tasks_.pop_front();
                }
            }
            t();
        }
    }
    bool lifo_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
};

} // namespace beluga

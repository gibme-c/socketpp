// Copyright (c) 2020-2026, Brandon Lehmann
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/**
 * @file thread_pool.cpp
 * @brief Implementation of the internal thread pool.
 */

#include "thread_pool.hpp"

#include <algorithm>

namespace socketpp::detail
{

    thread_pool::thread_pool(size_t num_threads, size_t max_queue_size): max_queue_size_(max_queue_size)
    {
        // Fall back to hardware concurrency with a floor of 2 so that at least
        // one worker is always available even if another is blocked in a callback.
        if (num_threads == 0)
        {
            num_threads = std::thread::hardware_concurrency();

            if (num_threads < 2)
                num_threads = 2;
        }

        workers_.reserve(num_threads);

        for (size_t i = 0; i < num_threads; ++i)
        {
            workers_.emplace_back(
                [this]()
                {
                    for (;;)
                    {
                        std::function<void()> task;

                        {
                            std::unique_lock<std::mutex> lock(mutex_);

                            cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });

                            // Drain remaining tasks before exiting on shutdown.
                            // Workers only exit when stopped_ is true AND the queue is empty.
                            if (stopped_ && queue_.empty())
                                return;

                            task = std::move(queue_.front());
                            queue_.pop_front();
                        }

                        task();
                    }
                });
        }
    }

    thread_pool::~thread_pool()
    {
        shutdown();
    }

    bool thread_pool::submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_)
                return false;

            // Backpressure: reject new tasks if the queue has reached its limit.
            if (max_queue_size_ > 0 && queue_.size() >= max_queue_size_)
                return false;

            queue_.push_back(std::move(task));
        }

        // Wake exactly one worker -- avoids thundering herd.
        cv_.notify_one();

        return true;
    }

    void thread_pool::shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_)
                return;

            stopped_ = true;
        }

        // Wake all workers so they can see stopped_ and drain remaining tasks.
        cv_.notify_all();

        for (auto &w : workers_)
        {
            if (w.joinable())
                w.join();
        }
    }

    size_t thread_pool::size() const noexcept
    {
        return workers_.size();
    }

} // namespace socketpp::detail

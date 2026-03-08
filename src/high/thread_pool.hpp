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
 * @file thread_pool.hpp
 * @brief Internal thread pool used by the high-level API to dispatch user callbacks.
 *
 * This is an implementation detail. All user-facing callbacks (on_data, on_close,
 * on_error, on_connect, on_message) are submitted to this pool so they never
 * execute on the event loop thread. This keeps the event loop responsive and
 * prevents user code from accidentally blocking I/O processing.
 */

#ifndef SOCKETPP_DETAIL_THREAD_POOL_HPP
#define SOCKETPP_DETAIL_THREAD_POOL_HPP

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace socketpp::detail
{

    /**
     * @brief A simple fixed-size thread pool with a shared task queue.
     *
     * Workers block on a condition variable until tasks are available. The pool
     * drains remaining tasks on shutdown before joining threads, ensuring that
     * in-flight callbacks complete.
     *
     * @note Not movable or copyable. Intended to be owned by a server or client
     *       impl via unique_ptr.
     */
    class thread_pool
    {
      public:
        /**
         * @brief Construct a thread pool and start worker threads.
         * @param num_threads Number of worker threads to spawn. If 0,
         *                    uses std::thread::hardware_concurrency() with a
         *                    minimum of 2.
         * @param max_queue_size Maximum number of pending tasks allowed in the
         *                       queue. submit() returns false if this limit would
         *                       be exceeded. 0 means unlimited (no backpressure).
         */
        explicit thread_pool(size_t num_threads = 0, size_t max_queue_size = 0);

        /**
         * @brief Destructor. Calls shutdown() if not already stopped.
         */
        ~thread_pool();

        thread_pool(const thread_pool &) = delete;
        thread_pool &operator=(const thread_pool &) = delete;
        thread_pool(thread_pool &&) = delete;
        thread_pool &operator=(thread_pool &&) = delete;

        /**
         * @brief Submit a task for execution on a worker thread.
         * @param task The callable to execute.
         * @return true if the task was enqueued, false if the pool is stopped.
         */
        bool submit(std::function<void()> task);

        /**
         * @brief Shut down the pool, drain remaining tasks, and join all workers.
         *
         * After shutdown(), submit() will return false. Safe to call multiple
         * times; subsequent calls are no-ops.
         */
        void shutdown();

        /**
         * @brief Get the number of worker threads.
         * @return The number of threads in the pool.
         */
        size_t size() const noexcept;

      private:
        std::vector<std::thread> workers_; ///< Worker threads.
        std::deque<std::function<void()>> queue_; ///< Pending task queue.
        mutable std::mutex mutex_; ///< Protects queue_ and stopped_.
        std::condition_variable cv_; ///< Signals workers when tasks arrive or shutdown begins.
        bool stopped_ = false; ///< Set to true on shutdown.
        size_t max_queue_size_ = 0; ///< Maximum queue depth (0 = unlimited).
    };

} // namespace socketpp::detail

#endif // SOCKETPP_DETAIL_THREAD_POOL_HPP

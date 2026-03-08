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
 * @file serial_queue.hpp
 * @brief Serialized execution adapter for thread_pool.
 *
 * Wraps a thread_pool with per-instance serialization: at most one callback
 * submitted through a given serial_queue instance executes at a time. Multiple
 * serial_queue instances sharing the same pool still run concurrently with
 * respect to each other.
 *
 * The internal state is ref-counted (shared_ptr) so that the drain loop
 * running on a pool thread survives even if the serial_queue object is
 * destroyed mid-drain (e.g. when a callback releases the last shared_ptr
 * to the owning connection).
 *
 * This is an internal header -- not part of the public API.
 */

#ifndef SOCKETPP_DETAIL_SERIAL_QUEUE_HPP
#define SOCKETPP_DETAIL_SERIAL_QUEUE_HPP

#include "thread_pool.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace socketpp::detail
{

    /**
     * @brief Serialized execution queue backed by a thread_pool.
     *
     * Ensures at most one task executes at a time per serial_queue instance.
     * Tasks are executed in FIFO order on the underlying thread_pool.
     */
    class serial_queue
    {
      public:
        explicit serial_queue(thread_pool *pool): state_(std::make_shared<state>(pool)) {}

        serial_queue(const serial_queue &) = delete;
        serial_queue &operator=(const serial_queue &) = delete;
        serial_queue(serial_queue &&) = delete;
        serial_queue &operator=(serial_queue &&) = delete;

        /**
         * @brief Submit a task for serialized execution.
         *
         * The task is appended to the internal queue. If no drain loop is
         * currently running, one is submitted to the pool.
         */
        void submit(std::function<void()> fn)
        {
            auto s = state_;

            std::lock_guard<std::mutex> lock(s->mutex_);
            s->pending_.push_back(std::move(fn));

            if (!s->draining_)
            {
                s->draining_ = true;
                s->pool_->submit([s]() { drain(s); });
            }
        }

      private:
        struct state
        {
            explicit state(thread_pool *pool) noexcept: pool_(pool) {}

            thread_pool *pool_;
            std::mutex mutex_;
            std::deque<std::function<void()>> pending_;
            bool draining_ = false; // guarded by mutex_, not atomic
        };

        /**
         * @brief Drain loop: pops and executes tasks one at a time.
         *
         * The shared_ptr parameter keeps the state alive for the duration of
         * the drain, even if the serial_queue object that spawned this drain
         * is destroyed by a callback (e.g. when the last shared_ptr to a
         * connection impl is released inside on_close).
         *
         * Re-entrance invariant: If a callback executing inside drain() calls
         * submit() on the same serial_queue, the submit just appends to
         * pending_ and does NOT trigger a second pool->submit() because
         * draining_ is already true. The existing drain loop picks up the new
         * item on its next iteration. This is correct and intentional -- do
         * not "fix" by adding a second drain submission.
         */
        static void drain(std::shared_ptr<state> s)
        {
            for (;;)
            {
                std::function<void()> fn;

                {
                    std::lock_guard<std::mutex> lock(s->mutex_);

                    if (s->pending_.empty())
                    {
                        s->draining_ = false;
                        return;
                    }

                    fn = std::move(s->pending_.front());
                    s->pending_.pop_front();
                }

                fn();
            }
        }

        std::shared_ptr<state> state_;
    };

} // namespace socketpp::detail

#endif // SOCKETPP_DETAIL_SERIAL_QUEUE_HPP

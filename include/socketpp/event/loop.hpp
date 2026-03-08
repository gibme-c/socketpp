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

/// @file loop.hpp
/// @brief Single-threaded event loop built on top of a platform dispatcher.
///
/// The event_loop drives the dispatcher's poll() in a loop and provides
/// higher-level conveniences: cross-thread callback posting, one-shot and
/// repeating timers, and a clean stop/shutdown mechanism.
///
/// Thread Safety:
///   - post() and stop() are safe to call from any thread.
///   - run(), run_once(), defer(), repeat(), and io() must be called from the
///     thread that owns the event loop (or from posted callbacks).
///   - The event_loop is non-copyable and non-movable.

#ifndef SOCKETPP_EVENT_LOOP_HPP
#define SOCKETPP_EVENT_LOOP_HPP

#include <atomic>
#include <chrono>
#include <socketpp/event/dispatcher.hpp>
#include <socketpp/event/timer.hpp>
#include <vector>

namespace socketpp
{

    // ── Event Loop ───────────────────────────────────────────────────────────────

    /// @brief Single-threaded, non-blocking event loop.
    ///
    /// Wraps a platform-specific dispatcher and provides run/stop lifecycle,
    /// cross-thread callback posting via a lock-free swap-buffer queue, and
    /// platform-native timer scheduling.
    class event_loop
    {
      public:
        /// @brief Constructs the event loop and its platform dispatcher.
        /// @throws std::runtime_error if the underlying OS resource cannot be created.
        event_loop();

        /// @brief Destroys the event loop.
        ///
        /// The loop must not be running when destroyed. Call stop() and join the
        /// background thread before destroying the event_loop.
        ~event_loop();

        event_loop(const event_loop &) = delete;
        event_loop &operator=(const event_loop &) = delete;
        event_loop(event_loop &&) = delete;
        event_loop &operator=(event_loop &&) = delete;

        // ── Run the event loop until stop() is called ────────────────────────

        /// @brief Runs the event loop, blocking until stop() is called or poll() fails.
        ///
        /// Each iteration drains posted callbacks, then calls poll(-1) which blocks
        /// indefinitely until an I/O event, timer, or wake() signal arrives. If
        /// poll() returns an error, the loop exits immediately.
        ///
        /// @note This method should typically be called on a dedicated thread.
        void run()
        {
            running_.store(true, std::memory_order_relaxed);
            stop_requested_.store(false, std::memory_order_relaxed);

            while (!stop_requested_.load(std::memory_order_relaxed))
            {
                drain_posts();

                auto r = dispatcher_->poll(-1);

                if (!r)
                    break;
            }

            running_.store(false, std::memory_order_relaxed);
        }

        // ── Run a single iteration of the event loop ─────────────────────────

        /// @brief Runs a single iteration of the event loop.
        ///
        /// Drains any pending posted callbacks, then calls poll() once with the
        /// given timeout. Useful for integrating the event loop into an external
        /// run loop or for testing.
        ///
        /// @param timeout Maximum time to wait for events. Defaults to 0 (non-blocking).
        void run_once(std::chrono::milliseconds timeout = std::chrono::milliseconds(0))
        {
            drain_posts();
            dispatcher_->poll(static_cast<int>(timeout.count()));
        }

        // ── Stop the event loop (thread-safe) ────────────────────────────────

        /// @brief Signals the event loop to stop.
        ///
        /// Sets the stop flag and wakes the dispatcher so that run() will exit
        /// at the next iteration boundary. This method is thread-safe and may be
        /// called from any thread.
        void stop() noexcept
        {
            stop_requested_.store(true, std::memory_order_relaxed);
            dispatcher_->wake();
        }

        // ── Check if the loop is currently running ───────────────────────────

        /// @brief Checks whether the loop is currently inside run().
        /// @return true if run() is executing, false otherwise.
        ///
        /// @note Uses relaxed memory ordering. The value may be stale if read from
        ///       a different thread without additional synchronization.
        bool running() const noexcept
        {
            return running_.load(std::memory_order_relaxed);
        }

        // ── Post a callback to run on the loop thread (thread-safe) ──────────

        /// @brief Posts a callback to be executed on the event loop thread.
        ///
        /// The callback is enqueued in a lock-free swap-buffer queue and the
        /// dispatcher is woken so that the callback is drained on the next
        /// iteration. This is the primary mechanism for safely dispatching work
        /// to the event loop from other threads.
        ///
        /// @param fn The callback to execute. Must not be null.
        ///
        /// @note Multiple calls to post() before the next drain are batched and
        ///       executed in FIFO order.
        void post(std::function<void()> fn)
        {
            spin_lock();
            active_queue_->push_back(std::move(fn));
            spin_unlock();

            dispatcher_->wake();
        }

        // ── Schedule a one-shot timer (platform-native) ──────────────────────

        /// @brief Schedules a one-shot timer.
        ///
        /// The callback fires once after @p delay milliseconds on the event loop
        /// thread. The returned handle can be used to cancel the timer before it
        /// fires.
        ///
        /// @param delay    Time to wait before firing.
        /// @param callback Function to invoke when the timer fires.
        /// @return A timer_handle for cancellation. Check with operator bool() -- a
        ///         false handle indicates the timer could not be created.
        timer_handle defer(std::chrono::milliseconds delay, std::function<void()> callback)
        {
            auto id = dispatcher_->schedule_timer(delay, false, std::move(callback));

            return timer_handle(id, [this](timer_id tid) { dispatcher_->cancel_timer(tid); });
        }

        // ── Schedule a repeating timer (platform-native) ─────────────────────

        /// @brief Schedules a repeating timer.
        ///
        /// The callback fires every @p interval milliseconds on the event loop
        /// thread until cancelled via the returned handle.
        ///
        /// @param interval Time between successive fires.
        /// @param callback Function to invoke each time the timer fires.
        /// @return A timer_handle for cancellation.
        timer_handle repeat(std::chrono::milliseconds interval, std::function<void()> callback)
        {
            auto id = dispatcher_->schedule_timer(interval, true, std::move(callback));

            return timer_handle(id, [this](timer_id tid) { dispatcher_->cancel_timer(tid); });
        }

        // ── Access the underlying dispatcher for socket registration ─────────

        /// @brief Returns a reference to the underlying platform dispatcher.
        ///
        /// Use this to register sockets (add/modify/remove) for I/O event monitoring.
        /// The dispatcher reference is valid for the lifetime of the event_loop.
        ///
        /// @return Reference to the dispatcher.
        dispatcher &io() noexcept
        {
            return *dispatcher_;
        }

      private:
        std::unique_ptr<dispatcher> dispatcher_;
        std::atomic<bool> running_ {false};
        std::atomic<bool> stop_requested_ {false};

        // ── Swap-buffer post queue ───────────────────────────────────────────
        //
        // The post queue uses a double-buffer (swap-buffer) strategy to minimize
        // lock contention. Producers (post()) append to active_queue_ under the
        // spinlock. The consumer (drain_posts()) swaps active_queue_ and
        // drain_queue_ under the spinlock, then processes drain_queue_ without
        // holding any lock. This means producers only contend with the brief
        // swap, never with callback execution.

        std::atomic_flag post_lock_ = {};
        std::vector<std::function<void()>> *active_queue_;
        std::vector<std::function<void()>> *drain_queue_;
        std::vector<std::function<void()>> queue_a_;
        std::vector<std::function<void()>> queue_b_;

        /// Acquires the spinlock protecting the active post queue pointer.
        /// Uses platform-appropriate pause/yield hints to reduce contention.
        void spin_lock() noexcept
        {
            while (post_lock_.test_and_set(std::memory_order_acquire))
            {
#if defined(SOCKETPP_COMPILER_MSVC)
                _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }
        }

        /// Releases the spinlock.
        void spin_unlock() noexcept
        {
            post_lock_.clear(std::memory_order_release);
        }

        /// Swaps the active and drain queues, then executes all drained callbacks.
        /// Called at the top of each event loop iteration.
        void drain_posts()
        {
            spin_lock();
            std::swap(active_queue_, drain_queue_);
            spin_unlock();

            for (auto &fn : *drain_queue_)
            {
                fn();
            }

            drain_queue_->clear();
        }
    };

} // namespace socketpp

#endif // SOCKETPP_EVENT_LOOP_HPP

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

/// @file timer.hpp
/// @brief Timer identifier and RAII handle for platform-native event loop timers.
///
/// Provides timer_id (a plain integer alias) and timer_handle, a lightweight
/// value type that allows callers to cancel a scheduled timer. Timer handles
/// are returned by event_loop::defer() and event_loop::repeat().

#ifndef SOCKETPP_EVENT_TIMER_HPP
#define SOCKETPP_EVENT_TIMER_HPP

#include <cstdint>
#include <functional>

namespace socketpp
{

    // ── Timer Identifier ─────────────────────────────────────────────────────────

    /// @brief Opaque timer identifier.
    ///
    /// A value of 0 indicates an invalid or unset timer. On Linux, the timer_id
    /// corresponds to a timerfd file descriptor. On macOS, it is a monotonically
    /// increasing counter used as the kqueue ident. On Windows, it is an
    /// incrementing counter used to index into a timer info map.
    using timer_id = uint64_t;

    // ── Timer Handle ─────────────────────────────────────────────────────────────

    /// @brief RAII cancellable handle to a scheduled timer.
    ///
    /// Returned by event_loop::defer() and event_loop::repeat(). The handle
    /// stores the timer's ID and a cancellation function that, when invoked,
    /// removes the timer from the dispatcher.
    ///
    /// timer_handle is move-only. When the handle is destroyed, the timer is
    /// automatically cancelled if still active. Use release() to detach the
    /// handle without cancelling (fire-and-forget).
    ///
    /// @warning The handle holds a raw pointer to the event_loop's dispatcher.
    ///          Do not let the handle outlive the event_loop.
    class timer_handle
    {
      public:
        /// @brief Constructs an empty (invalid) timer handle.
        timer_handle() noexcept: id_(0), cancel_fn_(nullptr) {}

        /// @brief Destructor. Cancels the timer if still active.
        ~timer_handle() noexcept
        {
            cancel();
        }

        /// @brief Move constructor. Transfers ownership from other.
        timer_handle(timer_handle &&other) noexcept:
            id_(other.id_), cancel_fn_(std::move(other.cancel_fn_))
        {
            other.id_ = 0;
            other.cancel_fn_ = nullptr;
        }

        /// @brief Move assignment. Cancels any existing timer, then transfers ownership.
        timer_handle &operator=(timer_handle &&other) noexcept
        {
            if (this != &other)
            {
                cancel();
                id_ = other.id_;
                cancel_fn_ = std::move(other.cancel_fn_);
                other.id_ = 0;
                other.cancel_fn_ = nullptr;
            }

            return *this;
        }

        timer_handle(const timer_handle &) = delete;
        timer_handle &operator=(const timer_handle &) = delete;

        /// @brief Returns the underlying timer identifier.
        /// @return The timer_id, or 0 if the handle is empty or already cancelled.
        timer_id id() const noexcept
        {
            return id_;
        }

        /// @brief Cancels the timer.
        ///
        /// If the timer has already fired (one-shot), been cancelled, or been
        /// released, this is a no-op. After cancellation, the handle becomes
        /// empty (id() returns 0 and operator bool() returns false).
        void cancel() noexcept
        {
            if (cancel_fn_)
            {
                cancel_fn_(id_);
                cancel_fn_ = nullptr;
                id_ = 0;
            }
        }

        /// @brief Releases ownership of the timer without cancelling it.
        ///
        /// After this call the handle becomes empty. The timer continues to
        /// run until it fires (one-shot) or is cancelled via another mechanism.
        ///
        /// @return The timer_id that was held, or 0 if the handle was empty.
        timer_id release() noexcept
        {
            auto tid = id_;
            id_ = 0;
            cancel_fn_ = nullptr;
            return tid;
        }

        /// @brief Tests whether this handle refers to an active timer.
        /// @return true if the handle has a valid timer ID, false otherwise.
        explicit operator bool() const noexcept
        {
            return id_ != 0;
        }

      private:
        friend class event_loop;

        /// @brief Constructs a timer handle with the given ID and cancellation callback.
        /// @param id        The timer identifier assigned by the dispatcher.
        /// @param cancel_fn Function that cancels the timer via the dispatcher.
        timer_handle(timer_id id, std::function<void(timer_id)> cancel_fn) noexcept:
            id_(id), cancel_fn_(std::move(cancel_fn))
        {
        }

        timer_id id_;
        std::function<void(timer_id)> cancel_fn_;
    };

} // namespace socketpp

#endif // SOCKETPP_EVENT_TIMER_HPP

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

/// @file kqueue.cpp
/// @brief macOS kqueue-based dispatcher implementation.
///
/// Uses kqueue/kevent for socket I/O multiplexing and EVFILT_TIMER for
/// platform-native timers. The waker is implemented via an EVFILT_USER
/// kevent with a reserved ident (UINTPTR_MAX), avoiding the need for a
/// pipe or eventfd.
///
/// kqueue vs epoll differences:
///   - kqueue uses separate filter registrations per event type (EVFILT_READ,
///     EVFILT_WRITE), unlike epoll which uses a single bitmask per fd. This means
///     modify() must compute the delta between old and new interest sets and
///     add/delete individual filters.
///   - kqueue reports EV_EOF on the event itself (not as a separate event), which
///     maps to io_event::peer_shutdown.
///   - kqueue has built-in timer support via EVFILT_TIMER, so no timerfd is needed.
///
/// Interest tracking:
///   Because kqueue requires explicit EV_DELETE for filters that were previously
///   EV_ADD'd, this dispatcher maintains an interests_ map to track the current
///   interest set per socket. modify() diffs against this to issue only the
///   necessary kevent changes.

#include <socketpp/platform/detect.hpp>

#if defined(SOCKETPP_OS_MACOS)

#include "../platform/detect_internal.hpp"

#include <atomic>
#include <socketpp/event/dispatcher.hpp>
#include <stdexcept>
#include <unordered_map>

namespace socketpp::detail
{

    class kqueue_dispatcher final : public dispatcher
    {
      public:
        kqueue_dispatcher(): kq_(::kqueue())
        {
            if (kq_ < 0)
                throw std::runtime_error("socketpp: kqueue creation failed");

            // Register an EVFILT_USER event as the waker mechanism.
            // EV_CLEAR ensures the event auto-resets after delivery, so
            // multiple wake() calls between polls are coalesced.
            struct kevent ev;
            EV_SET(&ev, WAKER_IDENT, EVFILT_USER, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);

            if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0)
            {
                ::close(kq_);
                kq_ = -1;
                throw std::runtime_error("socketpp: kqueue waker registration failed");
            }
        }

        ~kqueue_dispatcher() override
        {
            if (kq_ >= 0)
                ::close(kq_);
        }

        result<void> add(socket_t fd, io_event interest, io_callback cb) override
        {
            auto res = apply_interest(fd, interest);

            if (!res)
                return res;

            callbacks_[fd] = std::move(cb);
            interests_[fd] = interest;

            return result<void>();
        }

        result<void> modify(socket_t fd, io_event interest) override
        {
            // Compute the delta between old and new interest sets so we only
            // issue EV_ADD or EV_DELETE for filters that actually changed.
            auto old_it = interests_.find(fd);
            io_event old_interest = (old_it != interests_.end()) ? old_it->second : io_event::none;

            const bool had_read = has_event(old_interest, io_event::readable);
            const bool had_write = has_event(old_interest, io_event::writable);
            const bool want_read = has_event(interest, io_event::readable);
            const bool want_write = has_event(interest, io_event::writable);

            struct kevent changes[2];
            int nchanges = 0;

            if (want_read && !had_read)
            {
                EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
                ++nchanges;
            }
            else if (!want_read && had_read)
            {
                EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                ++nchanges;
            }

            if (want_write && !had_write)
            {
                EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
                ++nchanges;
            }
            else if (!want_write && had_write)
            {
                EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                ++nchanges;
            }

            if (nchanges > 0)
            {
                if (::kevent(kq_, changes, nchanges, nullptr, 0, nullptr) < 0)
                    return normalize_error(last_socket_error());
            }

            interests_[fd] = interest;

            return result<void>();
        }

        result<void> remove(socket_t fd) override
        {
            // Delete only the filters that were actually registered, based on
            // our tracked interest set.
            auto it = interests_.find(fd);

            if (it != interests_.end())
            {
                struct kevent changes[2];
                int nchanges = 0;

                if (has_event(it->second, io_event::readable))
                {
                    EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                    ++nchanges;
                }

                if (has_event(it->second, io_event::writable))
                {
                    EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                    ++nchanges;
                }

                // Ignore errors from EV_DELETE -- the fd may already be closed.
                if (nchanges > 0)
                    (void)::kevent(kq_, changes, nchanges, nullptr, 0, nullptr);

                interests_.erase(it);
            }

            callbacks_.erase(fd);

            return result<void>();
        }

        result<int> poll(int timeout_ms) override
        {
            struct kevent events[256];
            struct timespec ts;
            struct timespec *ts_ptr = nullptr;

            // A negative timeout means block indefinitely (pass nullptr to kevent).
            if (timeout_ms >= 0)
            {
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
                ts_ptr = &ts;
            }

            const int n = ::kevent(kq_, nullptr, 0, events, 256, ts_ptr);

            if (n < 0)
            {
                if (errno == EINTR)
                    return 0;

                return normalize_error(last_socket_error());
            }

            for (int i = 0; i < n; ++i)
            {
                // Waker event -- skip silently.
                if (events[i].filter == EVFILT_USER && events[i].ident == WAKER_IDENT)
                    continue;

                // Timer event -- dispatch callback, clean up one-shots.
                if (events[i].filter == EVFILT_TIMER)
                {
                    const auto tid = static_cast<timer_id>(events[i].ident);
                    auto timer_it = timer_callbacks_.find(tid);

                    if (timer_it != timer_callbacks_.end())
                    {
                        timer_it->second.callback();

                        if (!timer_it->second.repeating)
                            timer_callbacks_.erase(timer_it);
                    }

                    continue;
                }

                // Socket event -- translate kevent filter/flags to io_event.
                // Note: unlike epoll which delivers a single event with a bitmask,
                // kqueue delivers separate events per filter. A single kevent
                // struct carries one filter (EVFILT_READ or EVFILT_WRITE).
                const auto fd = static_cast<socket_t>(events[i].ident);

                auto ev = io_event::none;

                if (events[i].filter == EVFILT_READ)
                    ev |= io_event::readable;

                if (events[i].filter == EVFILT_WRITE)
                    ev |= io_event::writable;

                if (events[i].flags & EV_EOF)
                    ev |= io_event::peer_shutdown;

                if (events[i].flags & EV_ERROR)
                    ev |= io_event::error;

                auto it = callbacks_.find(fd);

                if (it != callbacks_.end())
                {
                    auto cb = it->second;
                    cb(fd, ev);
                }
            }

            return n;
        }

        void wake() override
        {
            // Trigger the EVFILT_USER event registered during construction.
            // NOTE_TRIGGER causes the event to fire once; EV_CLEAR (set at
            // registration time) auto-resets it after delivery.
            struct kevent ev;
            EV_SET(&ev, WAKER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            (void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        }

        timer_id schedule_timer(std::chrono::milliseconds timeout, bool repeat, std::function<void()> callback) override
        {
            const auto id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

            struct kevent ev;
            uint16_t flags = EV_ADD | EV_ENABLE;

            // EV_ONESHOT causes the kernel to automatically delete the filter
            // after it fires once. For repeating timers, we omit it.
            if (!repeat)
                flags |= EV_ONESHOT;

            // NOTE_MSECONDS tells kqueue to interpret the data field as
            // milliseconds rather than the default (platform-dependent) unit.
#ifdef NOTE_MSECONDS
            const unsigned int timer_units = NOTE_MSECONDS;
#else
            const unsigned int timer_units = 0;
#endif
            EV_SET(
                &ev,
                static_cast<uintptr_t>(id),
                EVFILT_TIMER,
                flags,
                timer_units,
                static_cast<intptr_t>(timeout.count()),
                nullptr);

            if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0)
                return 0;

            timer_callbacks_[id] = timer_info {std::move(callback), repeat};

            return id;
        }

        void cancel_timer(timer_id id) override
        {
            auto it = timer_callbacks_.find(id);

            if (it != timer_callbacks_.end())
            {
                struct kevent ev;
                EV_SET(&ev, static_cast<uintptr_t>(id), EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
                (void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
                timer_callbacks_.erase(it);
            }
        }

      private:
        /// Reserved ident for the EVFILT_USER waker. Uses UINTPTR_MAX to avoid
        /// collision with valid socket descriptors or timer IDs.
        static constexpr uintptr_t WAKER_IDENT = UINTPTR_MAX;

        int kq_ = -1;
        std::unordered_map<socket_t, io_callback> callbacks_; ///< Socket fd -> user callback.
        std::unordered_map<socket_t, io_event>
            interests_; ///< Socket fd -> current interest set (for delta computation in modify).

        struct timer_info
        {
            std::function<void()> callback;
            bool repeating;
        };

        std::unordered_map<timer_id, timer_info> timer_callbacks_;
        std::atomic<timer_id> next_timer_id_ {1}; ///< Monotonic counter for timer IDs.

        /// Registers EVFILT_READ and/or EVFILT_WRITE for a socket based on the
        /// interest mask. Used by add() for the initial registration.
        result<void> apply_interest(socket_t fd, io_event interest)
        {
            struct kevent changes[2];
            int nchanges = 0;

            if (has_event(interest, io_event::readable))
            {
                EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
                ++nchanges;
            }

            if (has_event(interest, io_event::writable))
            {
                EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
                ++nchanges;
            }

            if (nchanges > 0)
            {
                if (::kevent(kq_, changes, nchanges, nullptr, 0, nullptr) < 0)
                    return normalize_error(last_socket_error());
            }

            return result<void>();
        }
    };

} // namespace socketpp::detail

// ── Factory registration ─────────────────────────────────────────────────────

namespace socketpp
{

    std::unique_ptr<dispatcher> dispatcher::create()
    {
        return std::make_unique<detail::kqueue_dispatcher>();
    }

} // namespace socketpp

#endif // SOCKETPP_OS_MACOS

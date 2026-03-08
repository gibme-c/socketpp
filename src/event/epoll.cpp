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

/// @file epoll.cpp
/// @brief Linux epoll-based dispatcher implementation.
///
/// Uses epoll_create1/epoll_ctl/epoll_wait for socket I/O multiplexing and
/// timerfd for platform-native timers. The waker is implemented via an eventfd
/// registered with the epoll instance.
///
/// All epoll registrations include EPOLLERR | EPOLLHUP | EPOLLRDHUP
/// unconditionally so that error/hangup conditions are always reported
/// regardless of the user's interest mask.

#include <socketpp/platform/detect.hpp>

#if defined(SOCKETPP_OS_LINUX)

#include "../platform/detect_internal.hpp"
#include "waker.hpp"

#include <atomic>
#include <socketpp/event/dispatcher.hpp>
#include <stdexcept>
#include <unordered_map>

namespace socketpp::detail
{

    class epoll_dispatcher final : public dispatcher
    {
      public:
        epoll_dispatcher(): epoll_fd_(::epoll_create1(EPOLL_CLOEXEC))
        {
            if (epoll_fd_ < 0)
                throw std::runtime_error("socketpp: epoll_create1 failed");

            auto waker_result = waker::create();

            if (!waker_result)
            {
                ::close(epoll_fd_);
                epoll_fd_ = -1;
                throw std::runtime_error("socketpp: waker creation failed");
            }

            waker_ = std::move(waker_result.value());

            // Register the waker's eventfd for EPOLLIN so that wake() calls
            // cause epoll_wait to return.
            struct epoll_event ev = {};
            ev.events = EPOLLIN;
            ev.data.fd = waker_.fd();

            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, waker_.fd(), &ev) < 0)
            {
                ::close(epoll_fd_);
                epoll_fd_ = -1;
                throw std::runtime_error("socketpp: epoll waker registration failed");
            }
        }

        ~epoll_dispatcher() override
        {
            // Close all timerfd descriptors before the epoll fd.
            for (auto &pair : timer_callbacks_)
                ::close(pair.first);

            if (epoll_fd_ >= 0)
                ::close(epoll_fd_);
        }

        result<void> add(socket_t fd, io_event interest, io_callback cb) override
        {
            struct epoll_event ev = {};
            ev.events = to_epoll_events(interest);
            ev.data.fd = fd;

            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
                return normalize_error(last_socket_error());

            callbacks_[fd] = std::move(cb);

            return result<void>();
        }

        result<void> modify(socket_t fd, io_event interest) override
        {
            struct epoll_event ev = {};
            ev.events = to_epoll_events(interest);
            ev.data.fd = fd;

            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
                return normalize_error(last_socket_error());

            return result<void>();
        }

        result<void> remove(socket_t fd) override
        {
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
                return normalize_error(last_socket_error());

            callbacks_.erase(fd);

            return result<void>();
        }

        result<int> poll(int timeout_ms) override
        {
            struct epoll_event events[256];

            const int n = ::epoll_wait(epoll_fd_, events, 256, timeout_ms);

            if (n < 0)
            {
                // EINTR is not an error -- a signal interrupted the wait.
                if (errno == EINTR)
                    return 0;

                return normalize_error(last_socket_error());
            }

            for (int i = 0; i < n; ++i)
            {
                const auto fd = events[i].data.fd;

                // Waker event -- drain and skip. No user callback.
                if (fd == waker_.fd())
                {
                    waker_.drain();
                    continue;
                }

                // Timer event -- the timerfd became readable when it expired.
                // Read the expiration count to reset the timerfd readability.
                auto timer_it = timer_callbacks_.find(fd);

                if (timer_it != timer_callbacks_.end())
                {
                    uint64_t expirations = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
                    (void)::read(fd, &expirations, sizeof(expirations));
#pragma GCC diagnostic pop

                    auto &info = timer_it->second;
                    info.callback();

                    // One-shot timers are cleaned up immediately after firing.
                    if (!info.repeating)
                    {
                        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                        ::close(fd);
                        timer_callbacks_.erase(timer_it);
                    }

                    continue;
                }

                // Regular socket event -- translate epoll flags and dispatch.
                const auto ev = from_epoll_events(events[i].events);
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
            waker_.wake();
        }

        timer_id schedule_timer(std::chrono::milliseconds timeout, bool repeat, std::function<void()> callback) override
        {
            // Create a timerfd using CLOCK_MONOTONIC so it is unaffected by
            // wall-clock adjustments. Non-blocking to avoid stalling if read
            // is called when no expiration has occurred.
            const int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

            if (tfd < 0)
                return 0;

            struct itimerspec its = {};
            const auto secs = timeout.count() / 1000;
            const auto nsecs = (timeout.count() % 1000) * 1000000L;

            its.it_value.tv_sec = static_cast<time_t>(secs);
            its.it_value.tv_nsec = static_cast<long>(nsecs);

            // For repeating timers, set the interval equal to the initial delay.
            if (repeat)
            {
                its.it_interval.tv_sec = its.it_value.tv_sec;
                its.it_interval.tv_nsec = its.it_value.tv_nsec;
            }

            if (::timerfd_settime(tfd, 0, &its, nullptr) < 0)
            {
                ::close(tfd);
                return 0;
            }

            struct epoll_event ev = {};
            ev.events = EPOLLIN;
            ev.data.fd = tfd;

            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tfd, &ev) < 0)
            {
                ::close(tfd);
                return 0;
            }

            // On Linux, the timer_id is the timerfd descriptor itself.
            const auto id = static_cast<timer_id>(tfd);

            timer_callbacks_[tfd] = timer_info {id, std::move(callback), repeat};

            return id;
        }

        void cancel_timer(timer_id id) override
        {
            const auto tfd = static_cast<int>(id);
            auto it = timer_callbacks_.find(tfd);

            if (it != timer_callbacks_.end())
            {
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tfd, nullptr);
                ::close(tfd);
                timer_callbacks_.erase(it);
            }
        }

      private:
        int epoll_fd_ = -1;
        waker waker_;
        std::unordered_map<socket_t, io_callback> callbacks_; ///< Socket fd -> user callback.

        struct timer_info
        {
            timer_id id;
            std::function<void()> callback;
            bool repeating;
        };

        std::unordered_map<int, timer_info> timer_callbacks_; ///< timerfd -> timer info.

        /// Translates io_event interest flags to epoll event flags.
        /// EPOLLERR, EPOLLHUP, and EPOLLRDHUP are always included so error
        /// conditions are reported regardless of the caller's interest mask.
        static uint32_t to_epoll_events(io_event interest) noexcept
        {
            uint32_t events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;

            if (has_event(interest, io_event::readable))
                events |= EPOLLIN;

            if (has_event(interest, io_event::writable))
                events |= EPOLLOUT;

            return events;
        }

        /// Translates epoll event flags back to io_event flags.
        static io_event from_epoll_events(uint32_t events) noexcept
        {
            auto result = io_event::none;

            if (events & EPOLLIN)
                result |= io_event::readable;

            if (events & EPOLLOUT)
                result |= io_event::writable;

            if (events & EPOLLERR)
                result |= io_event::error;

            if (events & EPOLLHUP)
                result |= io_event::hangup;

            if (events & EPOLLRDHUP)
                result |= io_event::peer_shutdown;

            return result;
        }
    };

} // namespace socketpp::detail

// ── Factory registration ─────────────────────────────────────────────────────

namespace socketpp
{

    std::unique_ptr<dispatcher> dispatcher::create()
    {
        return std::make_unique<detail::epoll_dispatcher>();
    }

} // namespace socketpp

#endif // SOCKETPP_OS_LINUX

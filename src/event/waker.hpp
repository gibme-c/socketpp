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

/// @file waker.hpp
/// @brief Cross-thread wake-up primitive for unblocking a dispatcher's poll().
///
/// The waker provides a platform-specific signaling mechanism that the
/// dispatcher uses to break out of a blocking poll() call. This is critical
/// for implementing thread-safe post() and stop() on the event loop.
///
/// Platform Mechanisms:
///   - Linux:   eventfd (EFD_NONBLOCK). write(1) to signal, read() to drain.
///   - macOS:   pipe (O_NONBLOCK). Write 1 byte to signal, read all bytes to drain.
///   - Windows: IOCP PostQueuedCompletionStatus. Posts a completion with WAKER_KEY;
///              consumed by GetQueuedCompletionStatusEx.
///
/// Thread Safety:
///   wake() is safe to call from any thread. drain() and fd() are intended for
///   the event loop thread only. Multiple concurrent wake() calls are coalesced
///   (the poll will unblock at least once, but not necessarily once per call).
///
/// Ownership:
///   The waker is move-only. On Linux and macOS it owns file descriptors that
///   are closed on destruction. On Windows it holds a non-owning IOCP handle
///   (the dispatcher owns the IOCP).

#ifndef SOCKETPP_SRC_EVENT_WAKER_HPP
#define SOCKETPP_SRC_EVENT_WAKER_HPP

#include "../platform/detect_internal.hpp"

#include <cstdint>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>
#include <utility>

namespace socketpp::detail
{

    /// @brief Platform-specific wake-up signaling primitive.
    ///
    /// Used internally by each dispatcher backend to implement the wake()
    /// method. The waker is registered with the platform's I/O multiplexer
    /// so that a signal from any thread causes poll() to return.
    class waker
    {
      public:
        /// @brief Creates a new waker instance.
        ///
        /// On Linux, creates an eventfd with EFD_NONBLOCK | EFD_CLOEXEC.
        /// On macOS, creates a pipe with both ends set to O_NONBLOCK | FD_CLOEXEC.
        /// On Windows, the waker is a no-op at creation time; call
        /// set_iocp_handle() after the IOCP handle is available.
        ///
        /// @return A result containing the waker on success, or an error code.
        static result<waker> create() noexcept
        {
            waker w;

#if defined(SOCKETPP_OS_LINUX)
            w.efd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

            if (w.efd_ < 0)
                return normalize_error(last_socket_error());
#elif defined(SOCKETPP_OS_MACOS)
            if (::pipe(w.pipe_fds_) != 0)
                return normalize_error(last_socket_error());

            // Set both pipe ends to non-blocking and close-on-exec.
            for (int i = 0; i < 2; ++i)
            {
                int flags = ::fcntl(w.pipe_fds_[i], F_GETFL, 0);

                if (flags < 0)
                {
                    ::close(w.pipe_fds_[0]);
                    ::close(w.pipe_fds_[1]);
                    w.pipe_fds_[0] = -1;
                    w.pipe_fds_[1] = -1;
                    return normalize_error(last_socket_error());
                }

                if (::fcntl(w.pipe_fds_[i], F_SETFL, flags | O_NONBLOCK) < 0)
                {
                    ::close(w.pipe_fds_[0]);
                    ::close(w.pipe_fds_[1]);
                    w.pipe_fds_[0] = -1;
                    w.pipe_fds_[1] = -1;
                    return normalize_error(last_socket_error());
                }

                if (::fcntl(w.pipe_fds_[i], F_SETFD, FD_CLOEXEC) < 0)
                {
                    ::close(w.pipe_fds_[0]);
                    ::close(w.pipe_fds_[1]);
                    w.pipe_fds_[0] = -1;
                    w.pipe_fds_[1] = -1;
                    return normalize_error(last_socket_error());
                }
            }
#elif defined(SOCKETPP_OS_WINDOWS)
            // Windows waker uses IOCP; the handle is set later via set_iocp_handle()
            // after the IOCP is created by the dispatcher constructor.
#endif

            return w;
        }

        /// @brief Default-constructs an invalid waker.
        waker() noexcept = default;

        /// @brief Move constructor. Transfers ownership of platform resources.
        waker(waker &&other) noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            efd_ = other.efd_;
            other.efd_ = -1;
#elif defined(SOCKETPP_OS_MACOS)
            pipe_fds_[0] = other.pipe_fds_[0];
            pipe_fds_[1] = other.pipe_fds_[1];
            other.pipe_fds_[0] = -1;
            other.pipe_fds_[1] = -1;
#elif defined(SOCKETPP_OS_WINDOWS)
            iocp_ = other.iocp_;
            other.iocp_ = nullptr;
#endif
        }

        /// @brief Move assignment. Closes any existing resources before taking ownership.
        waker &operator=(waker &&other) noexcept
        {
            if (this != &other)
            {
                close();

#if defined(SOCKETPP_OS_LINUX)
                efd_ = other.efd_;
                other.efd_ = -1;
#elif defined(SOCKETPP_OS_MACOS)
                pipe_fds_[0] = other.pipe_fds_[0];
                pipe_fds_[1] = other.pipe_fds_[1];
                other.pipe_fds_[0] = -1;
                other.pipe_fds_[1] = -1;
#elif defined(SOCKETPP_OS_WINDOWS)
                iocp_ = other.iocp_;
                other.iocp_ = nullptr;
#endif
            }

            return *this;
        }

        ~waker() noexcept
        {
            close();
        }

        waker(const waker &) = delete;
        waker &operator=(const waker &) = delete;

        /// @brief Signals the waker to unblock poll(). Thread-safe.
        ///
        /// On Linux, writes a uint64_t(1) to the eventfd. eventfd accumulates
        /// values, so multiple wake() calls before a drain() are harmless.
        /// On macOS, writes a single byte to the pipe's write end. The pipe
        /// buffer absorbs multiple writes; drain() reads them all.
        /// On Windows, posts a zero-byte completion packet with WAKER_KEY.
        void wake() noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            const uint64_t val = 1;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
            (void)::write(efd_, &val, sizeof(val));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#elif defined(SOCKETPP_OS_MACOS)
            const char byte = 1;
            (void)::write(pipe_fds_[1], &byte, 1);
#elif defined(SOCKETPP_OS_WINDOWS)
            if (iocp_ != nullptr)
                ::PostQueuedCompletionStatus(iocp_, 0, WAKER_KEY, nullptr);
#endif
        }

        /// @brief Consumes the wake signal so it does not re-trigger.
        ///
        /// Must be called from the event loop thread after poll() returns and
        /// identifies the waker's fd/event. On Linux, reads the eventfd counter
        /// back to zero. On macOS, drains all bytes from the pipe. On Windows,
        /// this is a no-op because the IOCP completion is consumed by
        /// GetQueuedCompletionStatusEx.
        void drain() noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            uint64_t val = 0;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
            (void)::read(efd_, &val, sizeof(val));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#elif defined(SOCKETPP_OS_MACOS)
            char buf[64];

            while (::read(pipe_fds_[0], buf, sizeof(buf)) > 0)
            {
            }
#elif defined(SOCKETPP_OS_WINDOWS)
            // No-op: IOCP completion is consumed during GetQueuedCompletionStatusEx
#endif
        }

        /// @brief Returns the file descriptor to register with the I/O multiplexer.
        ///
        /// On Linux, returns the eventfd. On macOS, returns the read end of the pipe.
        /// On Windows, returns invalid_socket (not used -- IOCP does not need an fd).
        ///
        /// @return The readable file descriptor, or invalid_socket on Windows.
        socket_t fd() const noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            return efd_;
#elif defined(SOCKETPP_OS_MACOS)
            return pipe_fds_[0];
#elif defined(SOCKETPP_OS_WINDOWS)
            return invalid_socket;
#endif
        }

#if defined(SOCKETPP_OS_WINDOWS)
        /// @brief Sentinel completion key used to identify waker completions in IOCP.
        static constexpr ULONG_PTR WAKER_KEY = 0xFFFFFFFF;

        /// @brief Associates this waker with an IOCP handle.
        ///
        /// Must be called before wake(). The waker does not own the IOCP handle;
        /// it is owned by the iocp_dispatcher.
        ///
        /// @param h The IOCP handle created by iocp_dispatcher.
        void set_iocp_handle(HANDLE h) noexcept
        {
            iocp_ = h;
        }
#endif

      private:
        /// Closes owned platform resources.
        void close() noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            if (efd_ >= 0)
            {
                ::close(efd_);
                efd_ = -1;
            }
#elif defined(SOCKETPP_OS_MACOS)
            if (pipe_fds_[0] >= 0)
            {
                ::close(pipe_fds_[0]);
                pipe_fds_[0] = -1;
            }

            if (pipe_fds_[1] >= 0)
            {
                ::close(pipe_fds_[1]);
                pipe_fds_[1] = -1;
            }
#elif defined(SOCKETPP_OS_WINDOWS)
            // Non-owning handle -- just clear the pointer.
            iocp_ = nullptr;
#endif
        }

#if defined(SOCKETPP_OS_LINUX)
        int efd_ = -1; ///< eventfd file descriptor.
#elif defined(SOCKETPP_OS_MACOS)
        int pipe_fds_[2] = {-1, -1}; ///< pipe fds: [0] = read end, [1] = write end.
#elif defined(SOCKETPP_OS_WINDOWS)
        HANDLE iocp_ = nullptr; ///< Non-owning handle to the IOCP instance.
#endif
    };

} // namespace socketpp::detail

#endif // SOCKETPP_SRC_EVENT_WAKER_HPP

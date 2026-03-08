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

/// @file dispatcher.hpp
/// @brief Abstract I/O event dispatcher interface and I/O event flag types.
///
/// Defines the platform-independent dispatcher interface that all backend
/// implementations (epoll, kqueue, IOCP) must satisfy. Also provides the
/// io_event bitmask enum, bitwise operators, and the io_callback type alias.
///
/// The dispatcher is the lowest-level abstraction over OS multiplexing APIs.
/// It is typically owned by an event_loop and should not be shared across
/// threads. All methods except wake() must be called from the event loop
/// thread.

#ifndef SOCKETPP_EVENT_DISPATCHER_HPP
#define SOCKETPP_EVENT_DISPATCHER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <socketpp/event/timer.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>

namespace socketpp
{

    // ── I/O Event Flags ──────────────────────────────────────────────────────────

    /// @brief Bitmask flags representing I/O readiness events on a socket.
    ///
    /// These flags are used both to express interest (what events to watch for)
    /// and to report occurred events in callbacks. The readable/writable flags
    /// are the primary interest flags; error, hangup, and peer_shutdown are
    /// always reported when they occur regardless of the interest mask.
    enum class io_event : uint32_t
    {
        none = 0, ///< No events.
        readable = 1 << 0, ///< Socket has data available for reading, or a new connection is pending (listeners).
        writable = 1 << 1, ///< Socket is ready for writing without blocking.
        error = 1 << 2, ///< An error condition exists on the socket.
        hangup = 1 << 3, ///< The socket has been hung up (local side).
        peer_shutdown = 1 << 4, ///< The remote peer has shut down its end of the connection (EOF).
    };

    // ── Bitwise Operators ────────────────────────────────────────────────────────

    /// @brief Bitwise OR for combining io_event flags.
    inline constexpr io_event operator|(io_event a, io_event b) noexcept
    {
        return static_cast<io_event>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// @brief Bitwise AND for testing io_event flags.
    inline constexpr io_event operator&(io_event a, io_event b) noexcept
    {
        return static_cast<io_event>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// @brief Bitwise NOT for inverting io_event flags.
    inline constexpr io_event operator~(io_event a) noexcept
    {
        return static_cast<io_event>(~static_cast<uint32_t>(a));
    }

    /// @brief Compound bitwise OR assignment.
    inline constexpr io_event &operator|=(io_event &a, io_event b) noexcept
    {
        return a = a | b;
    }

    /// @brief Compound bitwise AND assignment.
    inline constexpr io_event &operator&=(io_event &a, io_event b) noexcept
    {
        return a = a & b;
    }

    /// @brief Tests whether a specific flag is set in an io_event mask.
    /// @param mask The combined event mask to test.
    /// @param flag The individual flag to check for.
    /// @return true if @p flag is present in @p mask.
    inline constexpr bool has_event(io_event mask, io_event flag) noexcept
    {
        return (mask & flag) != io_event::none;
    }

    // ── Callback Type ────────────────────────────────────────────────────────────

    /// @brief Callback invoked when I/O events occur on a registered socket.
    ///
    /// The first argument is the socket descriptor that triggered the event.
    /// The second is a bitmask of the events that occurred.
    ///
    /// @warning Callbacks are invoked on the event loop thread. Long-running work
    ///          should be dispatched to a separate thread or thread pool.
    using io_callback = std::function<void(socket_t, io_event)>;

    // ── Abstract Dispatcher ──────────────────────────────────────────────────────

    /// @brief Abstract interface for platform-specific I/O event dispatchers.
    ///
    /// Each platform provides a concrete implementation:
    ///   - Linux: epoll_dispatcher (epoll_create1 / epoll_ctl / epoll_wait)
    ///   - macOS: kqueue_dispatcher (kqueue / kevent)
    ///   - Windows: iocp_dispatcher (CreateIoCompletionPort / GetQueuedCompletionStatusEx)
    ///
    /// Use dispatcher::create() to obtain the platform-appropriate implementation.
    ///
    /// Thread Safety:
    ///   - add(), modify(), remove(), poll(), schedule_timer(), and cancel_timer()
    ///     must be called from the event loop thread only.
    ///   - wake() is the only method that is safe to call from any thread. It
    ///     is the primary mechanism for cross-thread notification.
    ///
    /// Lifetime:
    ///   Sockets must be removed (via remove()) before being closed. Closing a
    ///   socket without removing it first leads to undefined behavior on some
    ///   platforms (particularly epoll, which auto-removes on close only when all
    ///   file descriptor duplicates are closed).
    class dispatcher
    {
      public:
        virtual ~dispatcher() = default;

        /// @brief Registers a socket for I/O event monitoring.
        ///
        /// @param fd       The socket descriptor to monitor.
        /// @param interest A bitmask of io_event flags specifying which events to watch.
        /// @param cb       The callback to invoke when matching events occur.
        /// @return result<void> on success, or an error code on failure.
        ///
        /// @warning On Windows (IOCP), a socket can only be associated with one IOCP
        ///          handle via CreateIoCompletionPort, and this association is permanent
        ///          for the lifetime of the socket. If a socket is removed with remove()
        ///          and then re-added, the IOCP association still exists. The
        ///          ERROR_INVALID_PARAMETER that results is treated as success internally.
        ///
        /// @note On Windows, UDP (SOCK_DGRAM) sockets and listening sockets are
        ///       polled via WSAPoll rather than the IOCP zero-byte read/write trick,
        ///       because the zero-byte WSARecv completes immediately on datagram
        ///       sockets even when no data is available, producing false readability.
        virtual result<void> add(socket_t fd, io_event interest, io_callback cb) = 0;

        /// @brief Changes the set of events being watched for a previously added socket.
        ///
        /// @param fd       The socket descriptor (must have been added with add()).
        /// @param interest The new io_event interest mask. Replaces the previous mask entirely.
        /// @return result<void> on success, or an error code on failure.
        virtual result<void> modify(socket_t fd, io_event interest) = 0;

        /// @brief Stops monitoring a socket and removes its callback.
        ///
        /// @param fd The socket descriptor to remove.
        /// @return result<void> on success, or an error code on failure.
        ///
        /// @note On Windows, this cancels any pending overlapped I/O via CancelIoEx
        ///       and removes internal tracking state, but does not un-associate
        ///       the socket from the IOCP handle (which is impossible by design).
        virtual result<void> remove(socket_t fd) = 0;

        /// @brief Waits for I/O events and dispatches callbacks.
        ///
        /// Blocks for up to @p timeout_ms milliseconds waiting for events. When
        /// events are available, the registered callbacks are invoked synchronously
        /// before poll() returns.
        ///
        /// @param timeout_ms Maximum time to wait in milliseconds.
        ///                   Pass -1 to block indefinitely (until an event or wake()).
        ///                   Pass 0 for a non-blocking check.
        /// @return The number of events dispatched, or an error code.
        ///
        /// @note On platforms where EINTR can interrupt the wait (Linux, macOS),
        ///       poll() returns 0 rather than an error when interrupted by a signal.
        virtual result<int> poll(int timeout_ms) = 0;

        /// @brief Wakes up a blocked poll() call from any thread.
        ///
        /// This is the only thread-safe method on dispatcher. It uses a
        /// platform-specific waker mechanism:
        ///   - Linux: writes to an eventfd registered with epoll
        ///   - macOS: triggers an EVFILT_USER kevent
        ///   - Windows: posts a completion packet to the IOCP handle
        ///
        /// Multiple concurrent wake() calls are coalesced -- the poll() will
        /// unblock at least once, but not necessarily once per wake() call.
        virtual void wake() = 0;

        /// @brief Schedules a timer using the platform's native timer mechanism.
        ///
        /// The timer callback is invoked on the event loop thread when poll()
        /// processes the timer event.
        ///
        /// Platform implementations:
        ///   - Linux: timerfd_create + timerfd_settime, registered with epoll
        ///   - macOS: EVFILT_TIMER kevent with NOTE_MSECONDS
        ///   - Windows: CreateThreadpoolTimer that posts a completion packet to IOCP
        ///
        /// @param timeout  Delay before the first (and, if repeating, each subsequent) fire.
        /// @param repeat   If true, the timer repeats every @p timeout milliseconds.
        ///                 If false, the timer fires once and is automatically cleaned up.
        /// @param callback Function invoked on the event loop thread when the timer fires.
        /// @return A non-zero timer_id on success, or 0 on failure.
        virtual timer_id
            schedule_timer(std::chrono::milliseconds timeout, bool repeat, std::function<void()> callback) = 0;

        /// @brief Cancels a previously scheduled timer.
        ///
        /// If the timer has already fired (one-shot) or been cancelled, this is a no-op.
        ///
        /// @param id The timer identifier returned by schedule_timer().
        virtual void cancel_timer(timer_id id) = 0;

        /// @brief Creates the platform-appropriate dispatcher implementation.
        ///
        /// @return A unique_ptr to an epoll_dispatcher (Linux), kqueue_dispatcher (macOS),
        ///         or iocp_dispatcher (Windows).
        /// @throws std::runtime_error if the underlying OS resource cannot be created.
        static std::unique_ptr<dispatcher> create();
    };

} // namespace socketpp

#endif // SOCKETPP_EVENT_DISPATCHER_HPP

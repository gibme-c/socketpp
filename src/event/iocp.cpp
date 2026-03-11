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

/// @file iocp.cpp
/// @brief Windows IOCP-based dispatcher implementation.
///
/// Uses I/O Completion Ports (CreateIoCompletionPort / GetQueuedCompletionStatusEx)
/// for socket I/O multiplexing and the Windows thread pool timer API
/// (CreateThreadpoolTimer) for platform-native timers.
///
/// IOCP Readiness Model:
///   IOCP is a completion-based model, not a readiness model like epoll/kqueue.
///   To emulate level-triggered readiness notifications, this implementation uses
///   the "zero-byte read/write" trick: arm_read() issues a zero-byte WSARecv, and
///   arm_write() issues a zero-byte WSASend. When the socket becomes readable/writable,
///   the IOCP completion fires. After each callback, the operation is re-armed if the
///   interest mask still includes that direction.
///
/// IOCP Re-association Gotcha:
///   A socket can only be associated with one IOCP handle via CreateIoCompletionPort,
///   and this association is permanent for the socket's lifetime. Calling remove()
///   only removes internal tracking state; it cannot detach the socket from the IOCP.
///   If the same socket is later re-added, CreateIoCompletionPort fails with
///   ERROR_INVALID_PARAMETER. This is expected and treated as success.
///
/// Listener Socket Handling:
///   Listening (TCP accept) sockets cannot use the zero-byte WSARecv/WSASend IOCP
///   trick because they need accept readiness, not data readiness. They are polled
///   via WSAPoll in poll_listeners(), which runs before the IOCP wait. When TCP
///   listeners are present, the IOCP timeout is capped at 10ms.
///
/// UDP (Datagram) Socket Handling:
///   UDP sockets use overlapped WSARecvFrom with real buffers (not zero-byte).
///   Zero-byte WSARecv on UDP completes immediately even without data (false
///   readability). With real buffers, the IOCP completion fires only when an
///   actual datagram arrives. Completed datagrams are buffered internally and
///   delivered to the socket layer via retrieve_dgram(). Multiple concurrent
///   overlapped receives (recv_slots) are maintained per socket to avoid dropping
///   datagrams during processing.
///
/// Timer Architecture:
///   Timers use CreateThreadpoolTimer. The timer callback runs on a Windows thread
///   pool thread and posts a completion packet (with TIMER_KEY) to the IOCP. The
///   event loop thread picks it up during GetQueuedCompletionStatusEx and dispatches
///   the user callback. This ensures user timer callbacks always execute on the
///   event loop thread. The timer_infos_ map is protected by a spinlock because it
///   is accessed from both the thread pool timer callback and the event loop thread.

#include <socketpp/platform/detect.hpp>

#if defined(SOCKETPP_OS_WINDOWS)

#include "../platform/detect_internal.hpp"
#include "../platform/spinlock.hpp"
#include "waker.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <socketpp/event/dispatcher.hpp>
#include <socketpp/net/address.hpp>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace socketpp::detail
{

    class iocp_dispatcher final : public dispatcher
    {
      public:
        iocp_dispatcher(): iocp_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1))
        {
            if (iocp_ == nullptr)
                throw std::runtime_error("socketpp: CreateIoCompletionPort failed");

            auto waker_result = waker::create();

            if (!waker_result)
            {
                ::CloseHandle(iocp_);
                iocp_ = nullptr;
                throw std::runtime_error("socketpp: waker creation failed");
            }

            waker_ = std::move(waker_result.value());
            waker_.set_iocp_handle(iocp_);
        }

        ~iocp_dispatcher() override
        {
            // Cancel all pending overlapped I/O before destroying state.
            // Both stream sockets and dgram sockets have pending overlapped ops.
            for (auto &pair : states_)
            {
                if (!pair.second->is_listener)
                    ::CancelIoEx(reinterpret_cast<HANDLE>(pair.first), nullptr);
            }

            states_.clear();

            // Shut down all thread pool timers. SetThreadpoolTimer(nullptr)
            // disarms the timer, WaitForThreadpoolTimerCallbacks ensures no
            // callback is in-flight, and CloseThreadpoolTimer releases it.
            {
                scoped_lock<spinlock> lock(timer_mutex_);

                for (auto &pair : timer_infos_)
                {
                    ::SetThreadpoolTimer(pair.second->tp_timer, nullptr, 0, 0);
                    ::WaitForThreadpoolTimerCallbacks(pair.second->tp_timer, TRUE);
                    ::CloseThreadpoolTimer(pair.second->tp_timer);
                }

                timer_infos_.clear();
            }

            if (iocp_ != nullptr)
                ::CloseHandle(iocp_);
        }

        result<void> add(socket_t fd, io_event interest, io_callback cb) override
        {
            auto state = std::make_unique<per_socket_state>();
            state->fd = fd;
            state->callback = std::move(cb);
            state->interest = interest;

            // Detect listening sockets via SO_ACCEPTCONN.
            BOOL accept_conn = FALSE;
            int opt_len = sizeof(accept_conn);

            if (::getsockopt(
                    static_cast<SOCKET>(fd),
                    SOL_SOCKET,
                    SO_ACCEPTCONN,
                    reinterpret_cast<char *>(&accept_conn),
                    &opt_len)
                    == 0
                && accept_conn)
            {
                state->is_listener = true;
            }

            // Detect UDP (datagram) sockets via SO_TYPE. Dgram sockets use
            // overlapped WSARecvFrom with real buffers instead of the zero-byte
            // WSARecv trick (which produces false readability on UDP).
            if (!state->is_listener)
            {
                int sock_type = 0;
                int type_len = sizeof(sock_type);

                if (::getsockopt(
                        static_cast<SOCKET>(fd), SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&sock_type), &type_len)
                        == 0
                    && sock_type == SOCK_DGRAM)
                {
                    state->is_dgram = true;
                }
            }

            // Associate the socket with the IOCP handle (stream and dgram sockets).
            if (!state->is_listener)
            {
                const auto key = static_cast<ULONG_PTR>(fd);
                HANDLE h = ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), iocp_, key, 0);

                if (h == nullptr)
                {
                    const DWORD err = ::GetLastError();

                    // IOCP association is permanent and cannot be undone. If the
                    // socket was previously add()'d then remove()'d, the IOCP
                    // binding still exists. Re-adding fails with
                    // ERROR_INVALID_PARAMETER, which we treat as success.
                    if (err != ERROR_INVALID_PARAMETER)
                        return std::error_code(static_cast<int>(err), std::system_category());
                }
            }

            auto *sp = state.get();
            states_[fd] = std::move(state);

            // Arm the initial overlapped operations based on interest.
            if (sp->is_dgram)
            {
                // Allocate recv slots and arm overlapped WSARecvFrom operations.
                if (has_event(interest, io_event::readable))
                {
                    sp->recv_slots = std::make_unique<per_socket_state::recv_slot[]>(
                        per_socket_state::DGRAM_RECV_SLOTS);

                    for (int i = 0; i < per_socket_state::DGRAM_RECV_SLOTS; ++i)
                        arm_recv_from(*sp, i);
                }
            }
            else if (!sp->is_listener)
            {
                if (has_event(interest, io_event::readable))
                    arm_read(*sp);

                if (has_event(interest, io_event::writable))
                    arm_write(*sp);
            }

            return result<void>();
        }

        result<void> modify(socket_t fd, io_event interest) override
        {
            auto it = states_.find(fd);

            if (it == states_.end())
                return make_error_code(errc::invalid_state);

            auto &state = *it->second;
            const auto old_interest = state.interest;
            state.interest = interest;

            // Listener sockets use WSAPoll; no arming needed.
            if (state.is_listener)
                return result<void>();

            if (state.is_dgram)
            {
                // Arm dgram recv slots if readable interest is newly added.
                if (has_event(interest, io_event::readable) && !has_event(old_interest, io_event::readable))
                {
                    if (!state.recv_slots)
                    {
                        state.recv_slots = std::make_unique<per_socket_state::recv_slot[]>(
                            per_socket_state::DGRAM_RECV_SLOTS);
                    }

                    for (int i = 0; i < per_socket_state::DGRAM_RECV_SLOTS; ++i)
                    {
                        if (!state.recv_slots[i].pending)
                            arm_recv_from(state, i);
                    }
                }

                return result<void>();
            }

            // Only arm new interests that weren't previously armed. Already-pending
            // operations for existing interests will re-arm themselves after completion.
            if (has_event(interest, io_event::readable) && !has_event(old_interest, io_event::readable))
                arm_read(state);

            if (has_event(interest, io_event::writable) && !has_event(old_interest, io_event::writable))
                arm_write(state);

            return result<void>();
        }

        result<void> remove(socket_t fd) override
        {
            auto it = states_.find(fd);

            if (it != states_.end())
            {
                // Cancel any pending overlapped I/O so completions don't arrive
                // after we destroy the per_socket_state (and its OVERLAPPED structs).
                // Both stream and dgram sockets have pending overlapped operations.
                if (!it->second->is_listener)
                    ::CancelIoEx(reinterpret_cast<HANDLE>(fd), nullptr);

                states_.erase(it);
            }

            return result<void>();
        }

        result<int> poll(int timeout_ms) override
        {
            int dispatched = 0;

            // Poll TCP listener sockets via WSAPoll first. If any are ready,
            // set the IOCP timeout to 0 so we don't block.
            dispatched += poll_listeners();

            DWORD iocp_timeout;

            if (dispatched > 0)
            {
                // Already dispatched events from listeners; don't block on IOCP.
                iocp_timeout = 0;
            }
            else if (has_listeners())
            {
                // Cap IOCP wait to 10ms so TCP listeners get polled frequently.
                constexpr DWORD listener_poll_ms = 10;

                if (timeout_ms < 0)
                    iocp_timeout = listener_poll_ms;
                else
                    iocp_timeout = (std::min)(static_cast<DWORD>(timeout_ms), listener_poll_ms);
            }
            else
            {
                // No TCP listeners -- IOCP timeout is unrestricted. Dgram and
                // stream completions wake IOCP immediately via their overlapped ops.
                iocp_timeout = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
            }

            OVERLAPPED_ENTRY entries[256];
            ULONG count = 0;

            const BOOL ok = ::GetQueuedCompletionStatusEx(iocp_, entries, 256, &count, iocp_timeout, FALSE);

            if (!ok)
            {
                const DWORD err = ::GetLastError();

                if (err != WAIT_TIMEOUT)
                    return std::error_code(static_cast<int>(err), std::system_category());
            }

            // Two-phase processing for dgram sockets: first buffer all completed
            // datagrams, then notify once per socket. This ensures the drain loop
            // in handle_readable() can retrieve all available datagrams in one pass.
            std::vector<socket_t> dgram_ready;

            for (ULONG i = 0; i < count; ++i)
            {
                const auto key = entries[i].lpCompletionKey;

                // Waker completion -- consume and skip.
                if (key == waker::WAKER_KEY)
                {
                    waker_.drain();
                    continue;
                }

                // Timer completion -- the thread pool timer callback posted this.
                if (key == TIMER_KEY)
                {
                    const auto tid = static_cast<timer_id>(reinterpret_cast<uintptr_t>(entries[i].lpOverlapped));

                    dispatch_timer(tid);
                    ++dispatched;
                    continue;
                }

                // Socket completion.
                const auto fd = static_cast<socket_t>(key);
                auto it = states_.find(fd);

                if (it == states_.end())
                    continue;

                auto &state = *it->second;
                auto *ov = entries[i].lpOverlapped;
                const DWORD bytes = entries[i].dwNumberOfBytesTransferred;

                // ── Dgram completion: buffer the datagram, defer notification ──
                if (state.is_dgram)
                {
                    // Find which recv_slot completed by matching OVERLAPPED pointer.
                    int slot_idx = -1;

                    if (state.recv_slots)
                    {
                        for (int s = 0; s < per_socket_state::DGRAM_RECV_SLOTS; ++s)
                        {
                            if (ov == &state.recv_slots[s].ov)
                            {
                                slot_idx = s;
                                break;
                            }
                        }
                    }

                    if (slot_idx < 0)
                        continue; // Stale/cancelled completion.

                    auto &slot = state.recv_slots[slot_idx];
                    slot.pending = false;

                    // Check for errors via GetOverlappedResult.
                    DWORD actual_bytes = 0;
                    DWORD flags = 0;

                    BOOL gor = ::WSAGetOverlappedResult(
                        static_cast<SOCKET>(fd), &slot.ov, &actual_bytes, FALSE, &flags);

                    if (gor && actual_bytes > 0)
                    {
                        // Buffer the completed datagram.
                        per_socket_state::dgram_entry entry;
                        entry.data.assign(slot.buffer, slot.buffer + actual_bytes);

                        sock_address sa;
                        std::memcpy(sa.data(), &slot.src_addr, slot.src_addr_len);
                        sa.set_size(static_cast<uint32_t>(slot.src_addr_len));
                        entry.source = sa;

                        state.dgram_queue.push_back(std::move(entry));
                    }

                    // Re-arm this slot for the next datagram.
                    if (has_event(state.interest, io_event::readable))
                        arm_recv_from(state, slot_idx);

                    // Track for deduped notification in Phase B.
                    if (!state.dgram_notified)
                    {
                        state.dgram_notified = true;
                        dgram_ready.push_back(fd);
                    }

                    continue;
                }

                // ── Stream completion: immediate callback ─────────────────────
                auto ev = io_event::none;

                if (ov == &state.read_ov)
                {
                    state.read_pending = false;
                    ev |= io_event::readable;

                    // A zero-byte read completing with 0 bytes transferred indicates
                    // the remote end has shut down (graceful close / EOF).
                    if (bytes == 0)
                        ev |= io_event::peer_shutdown;
                }
                else if (ov == &state.write_ov)
                {
                    state.write_pending = false;
                    ev |= io_event::writable;
                }

                // Copy callback before invoking: the callback may call remove(fd)
                // which destroys per_socket_state (and thus the std::function) mid-call.
                auto cb = state.callback;
                cb(fd, ev);
                ++dispatched;

                // Re-arm after callback. The callback may have removed the socket,
                // so re-lookup is required.
                it = states_.find(fd);

                if (it != states_.end())
                {
                    auto &s = *it->second;

                    if (has_event(s.interest, io_event::readable) && !s.read_pending)
                        arm_read(s);

                    if (has_event(s.interest, io_event::writable) && !s.write_pending)
                        arm_write(s);
                }
            }

            // Phase B: Notify dgram sockets that have buffered datagrams.
            for (auto fd : dgram_ready)
            {
                auto it = states_.find(fd);

                if (it == states_.end())
                    continue;

                auto &state = *it->second;
                state.dgram_notified = false;

                // Skip notification if readable interest was removed (e.g., paused).
                // Buffered datagrams remain in the queue and will be delivered when
                // interest is restored via modify().
                if (state.dgram_queue.empty() || !has_event(state.interest, io_event::readable))
                    continue;

                auto cb = state.callback;
                cb(fd, io_event::readable);
                ++dispatched;
            }

            return dispatched;
        }

        void wake() override
        {
            waker_.wake();
        }

        timer_id schedule_timer(std::chrono::milliseconds timeout, bool repeat, std::function<void()> callback) override
        {
            const auto id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);

            auto info = std::make_shared<timer_info>();
            info->id = id;
            info->callback = std::move(callback);
            info->repeating = repeat;
            info->iocp = iocp_;

            info->tp_timer = ::CreateThreadpoolTimer(timer_callback, info.get(), nullptr);

            if (info->tp_timer == nullptr)
                return 0;

            // Convert milliseconds to FILETIME (100-nanosecond intervals).
            // Negative value = relative time from now.
            FILETIME due_time;
            LARGE_INTEGER li;
            li.QuadPart = -(static_cast<LONGLONG>(timeout.count()) * 10000LL);
            due_time.dwLowDateTime = li.LowPart;
            due_time.dwHighDateTime = static_cast<DWORD>(li.HighPart);

            // period=0 means one-shot; period>0 sets the repeat interval in ms.
            const DWORD period = repeat ? static_cast<DWORD>(timeout.count()) : 0;

            ::SetThreadpoolTimer(info->tp_timer, &due_time, period, 0);

            {
                scoped_lock<spinlock> lock(timer_mutex_);
                timer_infos_[id] = info;
            }

            return id;
        }

        void cancel_timer(timer_id id) override
        {
            std::shared_ptr<timer_info> info;

            {
                scoped_lock<spinlock> lock(timer_mutex_);
                auto it = timer_infos_.find(id);

                if (it == timer_infos_.end())
                    return;

                info = std::move(it->second);
                timer_infos_.erase(it);
            }

            // Disarm, wait for any in-flight callback to finish, then close.
            ::SetThreadpoolTimer(info->tp_timer, nullptr, 0, 0);
            ::WaitForThreadpoolTimerCallbacks(info->tp_timer, TRUE);
            ::CloseThreadpoolTimer(info->tp_timer);
        }

      private:
        HANDLE iocp_ = nullptr;
        waker waker_;

        /// Sentinel completion key for timer events posted to the IOCP.
        /// Distinct from WAKER_KEY (0xFFFFFFFF) to avoid collision.
        /// Sentinel completion key for timer events posted to the IOCP.
        /// Uses the upper half of the 64-bit range to guarantee no collision with
        /// socket handles, which are small positive values on Windows.
        static constexpr ULONG_PTR TIMER_KEY = ~static_cast<ULONG_PTR>(1);

        /// Per-socket state for IOCP-tracked sockets. Each socket gets its own
        /// pair of OVERLAPPED structs for independent read/write arming.
        /// Dgram sockets additionally have recv_slots for overlapped WSARecvFrom
        /// and a dgram_queue for buffering completed datagrams.
        struct per_socket_state
        {
            socket_t fd = invalid_socket;
            io_callback callback;
            io_event interest = io_event::none;
            OVERLAPPED read_ov = {}; ///< Used by arm_read() for the zero-byte WSARecv (stream only).
            OVERLAPPED write_ov = {}; ///< Used by arm_write() for the zero-byte WSASend (stream only).
            bool read_pending = false; ///< True while a zero-byte WSARecv is in flight (stream only).
            bool write_pending = false; ///< True while a zero-byte WSASend is in flight (stream only).
            bool is_listener = false; ///< True for TCP listening sockets only (polled via WSAPoll).
            bool is_dgram = false; ///< True for UDP sockets (use overlapped WSARecvFrom).

            // ── Dgram-specific state ─────────────────────────────────────────

            /// Overlapped receive slot for WSARecvFrom. Each slot can hold one
            /// in-flight overlapped receive and its completed datagram data.
            struct recv_slot
            {
                OVERLAPPED ov = {};
                char buffer[65536]; ///< Max UDP datagram size.
                sockaddr_storage src_addr = {};
                INT src_addr_len = sizeof(sockaddr_storage);
                bool pending = false;
            };

            /// Completed datagram waiting for retrieval via retrieve_dgram().
            struct dgram_entry
            {
                std::vector<char> data;
                sock_address source;
            };

            static constexpr int DGRAM_RECV_SLOTS = 4; ///< Concurrent outstanding receives.
            std::unique_ptr<recv_slot[]> recv_slots; ///< Only allocated for dgram sockets.
            std::deque<dgram_entry> dgram_queue; ///< Buffered completed datagrams.
            bool dgram_notified = false; ///< Dedup flag for batched notification within one poll().
        };

        std::unordered_map<socket_t, std::unique_ptr<per_socket_state>> states_;

        /// Timer metadata shared between the thread pool callback and the
        /// event loop thread. Uses shared_ptr because the thread pool callback
        /// may still reference the info after the timer is cancelled.
        struct timer_info
        {
            timer_id id = 0;
            std::function<void()> callback;
            bool repeating = false;
            HANDLE iocp = nullptr; ///< Non-owning handle used by the TP callback to post completions.
            PTP_TIMER tp_timer = nullptr;
        };

        spinlock timer_mutex_; ///< Protects timer_infos_ (accessed from TP threads and loop thread).
        std::unordered_map<timer_id, std::shared_ptr<timer_info>> timer_infos_;
        std::atomic<timer_id> next_timer_id_ {1};

        /// Thread pool timer callback. Runs on a Windows TP thread, NOT on the
        /// event loop thread. Posts a completion packet to the IOCP so the actual
        /// user callback runs on the event loop thread during poll().
        /// The timer_id is passed through the OVERLAPPED* field via reinterpret_cast.
        static VOID CALLBACK timer_callback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER)
        {
            auto *info = static_cast<timer_info *>(context);

            if (info->iocp != nullptr)
            {
                ::PostQueuedCompletionStatus(
                    info->iocp, 0, TIMER_KEY, reinterpret_cast<LPOVERLAPPED>(static_cast<uintptr_t>(info->id)));
            }
        }

        /// Dispatches a timer callback on the event loop thread. For one-shot
        /// timers, removes the timer from the map and cleans up the TP timer
        /// after the callback returns.
        void dispatch_timer(timer_id id)
        {
            std::shared_ptr<timer_info> info;

            {
                scoped_lock<spinlock> lock(timer_mutex_);
                auto it = timer_infos_.find(id);

                if (it == timer_infos_.end())
                    return;

                info = it->second;

                if (!info->repeating)
                    timer_infos_.erase(it);
            }

            info->callback();

            if (!info->repeating)
            {
                ::SetThreadpoolTimer(info->tp_timer, nullptr, 0, 0);
                ::WaitForThreadpoolTimerCallbacks(info->tp_timer, TRUE);
                ::CloseThreadpoolTimer(info->tp_timer);
            }
        }

        /// Checks if any registered sockets are TCP listeners (WSAPoll-polled).
        bool has_listeners() const
        {
            for (const auto &pair : states_)
            {
                if (pair.second->is_listener)
                    return true;
            }

            return false;
        }

        /// Polls TCP listener sockets for readability using WSAPoll.
        /// Returns the number of callbacks dispatched. Uses a non-blocking poll
        /// (timeout=0) so it never blocks the event loop.
        int poll_listeners()
        {
            std::vector<WSAPOLLFD> pfds;
            std::vector<socket_t> listener_fds;

            for (auto &pair : states_)
            {
                auto &s = *pair.second;

                if (s.is_listener && has_event(s.interest, io_event::readable))
                {
                    WSAPOLLFD pfd = {};
                    pfd.fd = static_cast<SOCKET>(s.fd);
                    pfd.events = POLLRDNORM;
                    pfds.push_back(pfd);
                    listener_fds.push_back(s.fd);
                }
            }

            if (pfds.empty())
                return 0;

            const int ready = ::WSAPoll(pfds.data(), static_cast<ULONG>(pfds.size()), 0);

            if (ready <= 0)
                return 0;

            int dispatched = 0;

            for (size_t i = 0; i < pfds.size(); ++i)
            {
                if (pfds[i].revents & (POLLRDNORM | POLLERR | POLLHUP))
                {
                    auto it = states_.find(listener_fds[i]);

                    if (it == states_.end())
                        continue;

                    auto ev = io_event::readable;

                    if (pfds[i].revents & POLLERR)
                        ev |= io_event::error;

                    auto cb = it->second->callback;
                    cb(listener_fds[i], ev);
                    ++dispatched;
                }
            }

            return dispatched;
        }

        /// Arms a zero-byte WSARecv to detect read readiness via IOCP.
        /// When data arrives (or the peer disconnects), the completion fires
        /// and poll() dispatches the callback. The read is re-armed after each
        /// dispatch if readable interest persists.
        void arm_read(per_socket_state &state)
        {
            if (state.read_pending || state.is_listener || state.is_dgram)
                return;

            ::ZeroMemory(&state.read_ov, sizeof(state.read_ov));

            WSABUF buf = {};
            buf.buf = nullptr;
            buf.len = 0;
            DWORD bytes = 0;
            DWORD flags = 0;

            const int ret = ::WSARecv(static_cast<SOCKET>(state.fd), &buf, 1, &bytes, &flags, &state.read_ov, nullptr);

            if (ret == SOCKET_ERROR)
            {
                const int err = ::WSAGetLastError();

                if (err == WSA_IO_PENDING)
                {
                    state.read_pending = true;
                }
                else
                {
                    // Immediate error -- report it to the callback.
                    state.callback(state.fd, io_event::error);
                }
            }
            else
            {
                // Completed synchronously -- a completion packet is still posted
                // to the IOCP, so mark as pending.
                state.read_pending = true;
            }
        }

        /// Arms a zero-byte WSASend to detect write readiness via IOCP.
        /// Same pattern as arm_read() but for the write direction.
        void arm_write(per_socket_state &state)
        {
            if (state.write_pending || state.is_listener || state.is_dgram)
                return;

            ::ZeroMemory(&state.write_ov, sizeof(state.write_ov));

            WSABUF buf = {};
            buf.buf = nullptr;
            buf.len = 0;
            DWORD bytes = 0;
            DWORD flags = 0;

            const int ret = ::WSASend(static_cast<SOCKET>(state.fd), &buf, 1, &bytes, flags, &state.write_ov, nullptr);

            if (ret == SOCKET_ERROR)
            {
                const int err = ::WSAGetLastError();

                if (err == WSA_IO_PENDING)
                {
                    state.write_pending = true;
                }
                else
                {
                    state.callback(state.fd, io_event::error);
                }
            }
            else
            {
                state.write_pending = true;
            }
        }

        /// Arms an overlapped WSARecvFrom on a dgram recv_slot.
        /// Uses a real buffer (65KB) so the completion only fires when an actual
        /// datagram arrives (unlike zero-byte WSARecv which gives false readability).
        void arm_recv_from(per_socket_state &state, int slot_index)
        {
            auto &slot = state.recv_slots[slot_index];

            if (slot.pending)
                return;

            ::ZeroMemory(&slot.ov, sizeof(slot.ov));
            slot.src_addr_len = sizeof(sockaddr_storage);

            WSABUF wsa_buf = {};
            wsa_buf.buf = slot.buffer;
            wsa_buf.len = sizeof(slot.buffer);
            DWORD bytes = 0;
            DWORD flags = 0;

            const int ret = ::WSARecvFrom(
                static_cast<SOCKET>(state.fd),
                &wsa_buf,
                1,
                &bytes,
                &flags,
                reinterpret_cast<sockaddr *>(&slot.src_addr),
                &slot.src_addr_len,
                &slot.ov,
                nullptr);

            if (ret == SOCKET_ERROR)
            {
                const int err = ::WSAGetLastError();

                if (err == WSA_IO_PENDING)
                {
                    slot.pending = true;
                }
                // else: immediate error, slot stays unarmed; will retry on next poll cycle.
            }
            else
            {
                // Completed synchronously -- a completion packet is still posted
                // to IOCP (we do NOT use FILE_SKIP_COMPLETION_PORT_ON_SUCCESS on
                // dgram sockets due to known Windows kernel issues).
                slot.pending = true;
            }
        }

        /// Retrieves a pre-buffered datagram from the dgram_queue.
        /// Called by udp_socket_base::recv_from_raw() on Windows when the socket
        /// is registered with this IOCP dispatcher.
        result<size_t> retrieve_dgram(socket_t fd, void *buf, size_t len, sock_address &src_out) override
        {
            auto it = states_.find(fd);

            if (it == states_.end() || !it->second->is_dgram || it->second->dgram_queue.empty())
                return make_error_code(errc::would_block);

            auto &front = it->second->dgram_queue.front();
            size_t copy_len = (std::min)(len, front.data.size());
            std::memcpy(buf, front.data.data(), copy_len);
            src_out = front.source;
            it->second->dgram_queue.pop_front();

            return copy_len;
        }
    };

} // namespace socketpp::detail

// ── Factory registration ─────────────────────────────────────────────────────

namespace socketpp
{

    std::unique_ptr<dispatcher> dispatcher::create()
    {
        return std::make_unique<detail::iocp_dispatcher>();
    }

} // namespace socketpp

#endif // SOCKETPP_OS_WINDOWS

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

#ifndef SOCKETPP_SOCKET_SOCKET_HPP
#define SOCKETPP_SOCKET_SOCKET_HPP

#include <atomic>
#include <functional>
#include <socketpp/net/address.hpp>
#include <socketpp/platform/detect.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>
#include <utility>

namespace socketpp
{

    // Forward declarations
    class dispatcher;
    class socket_options;

    class socket
    {
      public:
        // ── Factory (implemented in socket.cpp) ─────────────────────────────

        static result<socket> create(address_family af, socket_type type, int protocol = 0) noexcept;

        // ── Constructors / Assignment ────────────────────────────────────────

        socket() noexcept = default;

        explicit socket(socket_t handle) noexcept: handle_(handle) {}

        socket(socket &&other) noexcept:
            handle_(std::exchange(other.handle_, invalid_socket)),
            dispatcher_(std::exchange(other.dispatcher_, nullptr)),
            on_close_deregister_(std::move(other.on_close_deregister_))
        {
        }

        socket &operator=(socket &&other) noexcept
        {
            if (this != &other)
            {
                close();

                handle_ = std::exchange(other.handle_, invalid_socket);
                dispatcher_ = std::exchange(other.dispatcher_, nullptr);
                on_close_deregister_ = std::move(other.on_close_deregister_);
            }

            return *this;
        }

        socket(const socket &) = delete;
        socket &operator=(const socket &) = delete;

        ~socket() noexcept
        {
            close();
        }

        // ── Handle Access ────────────────────────────────────────────────────

        SOCKETPP_FORCEINLINE socket_t native_handle() const noexcept
        {
            return handle_;
        }

        socket_t release() noexcept
        {
            if (on_close_deregister_)
            {
                on_close_deregister_(handle_);
                on_close_deregister_ = nullptr;
            }

            dispatcher_ = nullptr;

            return std::exchange(handle_, invalid_socket);
        }

        SOCKETPP_FORCEINLINE bool is_open() const noexcept
        {
            return handle_ != invalid_socket;
        }

        // ── Core Operations (implemented in socket.cpp) ─────────────────────

        result<void> close() noexcept;

        result<void> shutdown(shutdown_mode how) noexcept;

        result<void> bind(const sock_address &addr) noexcept;

        result<void> set_non_blocking(bool enable) noexcept;

        result<sock_address> local_address() const noexcept;

        result<sock_address> peer_address() const noexcept;

        result<void> apply(const socket_options &opts) noexcept;

      protected:
        socket_t handle_ = invalid_socket;
        dispatcher *dispatcher_ = nullptr;

        std::function<void(socket_t)> on_close_deregister_;

        void set_dispatcher(dispatcher *d) noexcept
        {
            dispatcher_ = d;
        }

        dispatcher *get_dispatcher() const noexcept
        {
            return dispatcher_;
        }

        void set_close_callback(std::function<void(socket_t)> fn) noexcept
        {
            on_close_deregister_ = std::move(fn);
        }

        mutable std::atomic_flag send_lock_ = {};
        mutable std::atomic_flag recv_lock_ = {};

        struct spin_guard
        {
            std::atomic_flag &flag_;

            spin_guard(std::atomic_flag &f) noexcept: flag_(f)
            {
                while (flag_.test_and_set(std::memory_order_acquire))
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

            ~spin_guard() noexcept
            {
                flag_.clear(std::memory_order_release);
            }

            spin_guard(const spin_guard &) = delete;
            spin_guard &operator=(const spin_guard &) = delete;
        };
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_SOCKET_HPP

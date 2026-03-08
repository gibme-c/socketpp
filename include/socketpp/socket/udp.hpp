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

#ifndef SOCKETPP_SOCKET_UDP_HPP
#define SOCKETPP_SOCKET_UDP_HPP

#include <functional>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/capabilities.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>

namespace socketpp
{

    class event_loop;

    // ── Batch Message Entry ──────────────────────────────────────────────────────

    struct msg_batch_entry
    {
        void *buf;
        size_t len;
        sock_address addr;
        size_t transferred;
    };

    // ── UDP Socket Base ──────────────────────────────────────────────────────────

    class udp_socket_base : public socket
    {
      public:
        udp_socket_base() noexcept = default;

        explicit udp_socket_base(socket_t handle) noexcept: socket(handle) {}

        explicit udp_socket_base(socket &&s) noexcept: socket(std::move(s)) {}

        udp_socket_base(udp_socket_base &&) noexcept = default;
        udp_socket_base &operator=(udp_socket_base &&) noexcept = default;

        udp_socket_base(const udp_socket_base &) = delete;
        udp_socket_base &operator=(const udp_socket_base &) = delete;

        result<void> open(address_family af, const sock_address &bind_addr, const socket_options &opts = {}) noexcept;

        result<size_t> send_to_raw(const void *data, size_t len, const sock_address &dest, int flags = 0) noexcept;

        result<size_t> recv_from_raw(void *buf, size_t len, sock_address &src_out, int flags = 0) noexcept;

        result<size_t> send_to_iov_raw(span<const iovec> bufs, const sock_address &dest, int flags = 0);

        result<size_t> recv_from_iov_raw(span<iovec> bufs, sock_address &src_out, int flags = 0);

        result<int> recv_batch(span<msg_batch_entry> msgs) noexcept;

        result<int> send_batch(span<msg_batch_entry> msgs) noexcept;
    };

    class udp4_socket : public udp_socket_base
    {
      public:
        using udp_socket_base::udp_socket_base;

        result<void> open(const inet4_address &bind_addr, const socket_options &opts = {}) noexcept;

        result<size_t> send_to(const void *data, size_t len, const inet4_address &dest, int flags = 0) noexcept;

        result<size_t> recv_from(void *buf, size_t len, inet4_address &src_out, int flags = 0) noexcept;

        result<size_t> send_to_iov(span<const iovec> bufs, const inet4_address &dest, int flags = 0);

        result<size_t> recv_from_iov(span<iovec> bufs, inet4_address &src_out, int flags = 0);

        result<inet4_address> local_addr() const noexcept;

        void recv_async(
            event_loop &loop,
            void *buf,
            size_t len,
            std::function<void(result<size_t>, inet4_address)> callback);
    };

    class udp6_socket : public udp_socket_base
    {
      public:
        using udp_socket_base::udp_socket_base;

        result<void> open(const inet6_address &bind_addr, const socket_options &opts = {}) noexcept;

        result<size_t> send_to(const void *data, size_t len, const inet6_address &dest, int flags = 0) noexcept;

        result<size_t> recv_from(void *buf, size_t len, inet6_address &src_out, int flags = 0) noexcept;

        result<size_t> send_to_iov(span<const iovec> bufs, const inet6_address &dest, int flags = 0);

        result<size_t> recv_from_iov(span<iovec> bufs, inet6_address &src_out, int flags = 0);

        result<inet6_address> local_addr() const noexcept;

        void recv_async(
            event_loop &loop,
            void *buf,
            size_t len,
            std::function<void(result<size_t>, inet6_address)> callback);
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_UDP_HPP

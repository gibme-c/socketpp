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

#ifndef SOCKETPP_SOCKET_TCP_HPP
#define SOCKETPP_SOCKET_TCP_HPP

#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/socket/socket.hpp>

namespace socketpp
{

    class tcp_socket : public socket
    {
      public:
        tcp_socket() noexcept = default;

        explicit tcp_socket(socket_t handle) noexcept: socket(handle) {}

        explicit tcp_socket(socket &&s) noexcept: socket(std::move(s)) {}

        tcp_socket(tcp_socket &&) noexcept = default;
        tcp_socket &operator=(tcp_socket &&) noexcept = default;

        tcp_socket(const tcp_socket &) = delete;
        tcp_socket &operator=(const tcp_socket &) = delete;

        result<size_t> send(const void *data, size_t len, int flags = 0) noexcept;

        result<size_t> recv(void *buf, size_t len, int flags = 0) noexcept;

        result<size_t> send_iov(span<const iovec> bufs);

        result<size_t> recv_iov(span<iovec> bufs);
    };

    class tcp4_socket : public tcp_socket
    {
      public:
        using tcp_socket::tcp_socket;

        tcp4_socket(tcp_socket &&s) noexcept: tcp_socket(std::move(s)) {}

        result<inet4_address> local_addr() const noexcept;

        result<inet4_address> peer_addr() const noexcept;
    };

    class tcp6_socket : public tcp_socket
    {
      public:
        using tcp_socket::tcp_socket;

        tcp6_socket(tcp_socket &&s) noexcept: tcp_socket(std::move(s)) {}

        result<inet6_address> local_addr() const noexcept;

        result<inet6_address> peer_addr() const noexcept;
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_TCP_HPP

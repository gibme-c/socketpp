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

#ifndef SOCKETPP_SOCKET_UDP_PEER_HPP
#define SOCKETPP_SOCKET_UDP_PEER_HPP

#include <cstdint>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/capabilities.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>

namespace socketpp
{

    class udp4_peer_socket
    {
      public:
        static result<udp4_peer_socket>
            create(const inet4_address &local, const inet4_address &peer, const socket_options &opts = {}) noexcept;

        udp4_peer_socket() noexcept;
        ~udp4_peer_socket() noexcept;

        udp4_peer_socket(udp4_peer_socket &&other) noexcept;
        udp4_peer_socket &operator=(udp4_peer_socket &&other) noexcept;

        udp4_peer_socket(const udp4_peer_socket &) = delete;
        udp4_peer_socket &operator=(const udp4_peer_socket &) = delete;

        result<size_t> send(const void *data, size_t len, int flags = 0) noexcept;
        result<size_t> recv(void *buf, size_t len, int flags = 0) noexcept;

        const inet4_address &peer_addr() const noexcept
        {
            return peer_;
        }

        static constexpr bool kernel_demux_available() noexcept
        {
            return has_kernel_udp_demux;
        }

        socket_t native_handle() const noexcept;

        bool is_open() const noexcept;

      private:
        inet4_address peer_;
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    class udp6_peer_socket
    {
      public:
        static result<udp6_peer_socket>
            create(const inet6_address &local, const inet6_address &peer, const socket_options &opts = {}) noexcept;

        udp6_peer_socket() noexcept;
        ~udp6_peer_socket() noexcept;

        udp6_peer_socket(udp6_peer_socket &&other) noexcept;
        udp6_peer_socket &operator=(udp6_peer_socket &&other) noexcept;

        udp6_peer_socket(const udp6_peer_socket &) = delete;
        udp6_peer_socket &operator=(const udp6_peer_socket &) = delete;

        result<size_t> send(const void *data, size_t len, int flags = 0) noexcept;
        result<size_t> recv(void *buf, size_t len, int flags = 0) noexcept;

        const inet6_address &peer_addr() const noexcept
        {
            return peer_;
        }

        static constexpr bool kernel_demux_available() noexcept
        {
            return has_kernel_udp_demux;
        }

        socket_t native_handle() const noexcept;

        bool is_open() const noexcept;

      private:
        inet6_address peer_;
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_UDP_PEER_HPP

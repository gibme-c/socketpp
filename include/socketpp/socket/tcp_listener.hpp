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

#ifndef SOCKETPP_SOCKET_TCP_LISTENER_HPP
#define SOCKETPP_SOCKET_TCP_LISTENER_HPP

#include <functional>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/tcp.hpp>

namespace socketpp
{

    class event_loop;

    class tcp_listener_base
    {
      public:
        tcp_listener_base() noexcept = default;
        ~tcp_listener_base() noexcept = default;

        tcp_listener_base(tcp_listener_base &&) noexcept = default;
        tcp_listener_base &operator=(tcp_listener_base &&) noexcept = default;

        tcp_listener_base(const tcp_listener_base &) = delete;
        tcp_listener_base &operator=(const tcp_listener_base &) = delete;

        result<void>
            open(address_family af, const sock_address &bind_addr, int backlog, const socket_options &opts) noexcept;

        result<tcp_socket> accept_raw() noexcept;

        result<tcp_socket> accept_raw(sock_address &peer_out) noexcept;

        void set_accepted_options(const socket_options &opts) noexcept
        {
            accepted_opts_ = opts;
            accepted_opts_empty_ = false;
        }

        const socket &handle() const noexcept
        {
            return socket_;
        }

        socket &handle() noexcept
        {
            return socket_;
        }

        void stop() noexcept
        {
            socket_.close();
        }

      protected:
        socket socket_;
        socket_options accepted_opts_;
        bool accepted_opts_empty_ = true;
    };

    class tcp4_listener : public tcp_listener_base
    {
      public:
        result<void> open(
            const inet4_address &bind_addr,
            int backlog = default_backlog,
            const socket_options &opts = {}) noexcept;

        result<tcp4_socket> accept() noexcept;

        result<tcp4_socket> accept(inet4_address &peer_out) noexcept;

        void accept_loop(event_loop &loop, std::function<void(result<tcp4_socket>, inet4_address)> handler);
    };

    class tcp6_listener : public tcp_listener_base
    {
      public:
        result<void> open(
            const inet6_address &bind_addr,
            int backlog = default_backlog,
            const socket_options &opts = {}) noexcept;

        result<tcp6_socket> accept() noexcept;

        result<tcp6_socket> accept(inet6_address &peer_out) noexcept;

        void accept_loop(event_loop &loop, std::function<void(result<tcp6_socket>, inet6_address)> handler);
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_TCP_LISTENER_HPP

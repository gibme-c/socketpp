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

#ifndef SOCKETPP_SOCKET_TCP_CONNECTOR_HPP
#define SOCKETPP_SOCKET_TCP_CONNECTOR_HPP

#include <chrono>
#include <functional>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/tcp.hpp>

namespace socketpp
{

    class event_loop;

    struct tcp4_connector
    {
        static result<tcp4_socket> connect(const inet4_address &addr, const socket_options &opts = {}) noexcept;

        static void connect_async(
            event_loop &loop,
            const inet4_address &addr,
            std::function<void(result<tcp4_socket>)> callback,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
            const socket_options &opts = {});
    };

    struct tcp6_connector
    {
        static result<tcp6_socket> connect(const inet6_address &addr, const socket_options &opts = {}) noexcept;

        static void connect_async(
            event_loop &loop,
            const inet6_address &addr,
            std::function<void(result<tcp6_socket>)> callback,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
            const socket_options &opts = {});
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_TCP_CONNECTOR_HPP

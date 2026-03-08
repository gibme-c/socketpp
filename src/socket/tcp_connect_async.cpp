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

#include "../platform/detect_internal.hpp"

#include <memory>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/tcp_connector.hpp>

namespace socketpp
{

    namespace
    {

        template<typename Connector, typename Socket, typename Addr>
        void connect_async_impl(
            event_loop &loop,
            const Addr &addr,
            std::function<void(result<Socket>)> callback,
            std::chrono::milliseconds timeout,
            const socket_options &opts)
        {
            auto r = Connector::connect(addr, opts);

            if (!r)
            {
                callback(r.error());
                return;
            }

            struct connect_state
            {
                Socket sock;
                std::function<void(result<Socket>)> callback;
                timer_handle timer;
                bool completed = false;

                explicit connect_state(Socket &&s, std::function<void(result<Socket>)> cb):
                    sock(std::move(s)), callback(std::move(cb))
                {
                }
            };

            auto state = std::make_shared<connect_state>(Socket(std::move(r.value())), std::move(callback));
            const auto fd = state->sock.native_handle();

            state->timer = loop.defer(
                timeout,
                [state, &loop, fd]()
                {
                    if (state->completed)
                        return;

                    state->completed = true;

                    loop.io().remove(fd);
                    state->sock.close();

                    state->callback(make_error_code(errc::timed_out));
                });

            loop.io().add(
                fd,
                io_event::writable,
                [state, &loop](socket_t cb_fd, io_event /*events*/)
                {
                    if (state->completed)
                        return;

                    state->completed = true;

                    state->timer.cancel();

                    loop.io().remove(cb_fd);

                    int so_error = 0;
                    socklen_t len = sizeof(so_error);

#if defined(SOCKETPP_OS_WINDOWS)
                    if (::getsockopt(
                            static_cast<SOCKET>(cb_fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len)
                        != 0)
#else
                    if (::getsockopt(
                            static_cast<int>(cb_fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len)
                        != 0)
#endif
                    {
                        auto ec = normalize_error(last_socket_error());
                        state->sock.close();
                        state->callback(ec);
                        return;
                    }

                    if (so_error != 0)
                    {
                        // Use the appropriate error category for SO_ERROR on each platform.
                        // to ensure consistent error normalization
                        auto raw_ec = std::error_code(
                            so_error,
#if defined(SOCKETPP_OS_WINDOWS)
                            std::system_category()
#else
                            std::generic_category()
#endif
                        );

                        auto ec = normalize_error(raw_ec);
                        state->sock.close();
                        state->callback(ec);
                        return;
                    }

                    state->callback(std::move(state->sock));
                });
        }

    } // namespace

    void tcp4_connector::connect_async(
        event_loop &loop,
        const inet4_address &addr,
        std::function<void(result<tcp4_socket>)> callback,
        std::chrono::milliseconds timeout,
        const socket_options &opts)
    {
        connect_async_impl<tcp4_connector, tcp4_socket, inet4_address>(loop, addr, std::move(callback), timeout, opts);
    }

    void tcp6_connector::connect_async(
        event_loop &loop,
        const inet6_address &addr,
        std::function<void(result<tcp6_socket>)> callback,
        std::chrono::milliseconds timeout,
        const socket_options &opts)
    {
        connect_async_impl<tcp6_connector, tcp6_socket, inet6_address>(loop, addr, std::move(callback), timeout, opts);
    }

} // namespace socketpp

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

#include <chrono>
#include <memory>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/tcp_listener.hpp>

namespace socketpp
{

    namespace
    {

        template<typename Listener, typename Socket, typename Addr>
        void accept_loop_impl(Listener &listener, event_loop &loop, std::function<void(result<Socket>, Addr)> handler)
        {
            const auto fd = listener.handle().native_handle();

            auto backoff_ms = std::make_shared<int>(100);

            auto cb = [&listener, &loop, handler, backoff_ms](socket_t /*fd*/, io_event events)
            {
                if (!has_event(events, io_event::readable))
                    return;

                for (;;)
                {
                    Addr peer;
                    auto r = listener.accept(peer);

                    if (!r)
                    {
                        const auto ec = r.error();

                        if (ec == make_error_code(errc::would_block))
                            break;

                        if (ec == make_error_code(errc::fd_limit_reached))
                        {
                            const auto listen_fd = listener.handle().native_handle();

                            loop.io().modify(listen_fd, io_event::none);

                            const int delay = *backoff_ms;
                            *backoff_ms = std::min(*backoff_ms * 2, 5000);

                            loop.defer(
                                std::chrono::milliseconds(delay),
                                [&loop, listen_fd, backoff_ms]() { loop.io().modify(listen_fd, io_event::readable); }).release();

                            handler(ec, Addr {});

                            return;
                        }

                        handler(ec, Addr {});

                        break;
                    }

                    *backoff_ms = 100;

                    handler(std::move(r), peer);
                }
            };

            loop.io().add(fd, io_event::readable, std::move(cb));
        }

    } // namespace

    void tcp4_listener::accept_loop(event_loop &loop, std::function<void(result<tcp4_socket>, inet4_address)> handler)
    {
        accept_loop_impl<tcp4_listener, tcp4_socket, inet4_address>(*this, loop, std::move(handler));
    }

    void tcp6_listener::accept_loop(event_loop &loop, std::function<void(result<tcp6_socket>, inet6_address)> handler)
    {
        accept_loop_impl<tcp6_listener, tcp6_socket, inet6_address>(*this, loop, std::move(handler));
    }

} // namespace socketpp

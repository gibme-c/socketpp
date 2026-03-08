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

/**
 * @file udp_recv_async.cpp
 * @brief One-shot asynchronous UDP receive via the event loop.
 *
 * Registers the socket for readability on the event loop. When data arrives,
 * the registration is removed and the callback is invoked exactly once. This
 * provides a simple "receive one datagram" async primitive.
 *
 * @warning The `this` pointer and buffer are captured by the callback lambda.
 *          Both the socket and the buffer must remain valid until the callback fires.
 *
 * @warning On Windows with IOCP, the zero-byte WSARecv trick completes immediately
 *          on UDP sockets (even without data), giving false readability. The event
 *          loop's IOCP backend handles this by using WSAPoll for SOCK_DGRAM sockets.
 */

#include <cstring>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/udp.hpp>

namespace socketpp
{

    // One-shot async receive: register for readable, then deregister on first callback.
    void udp4_socket::recv_async(
        event_loop &loop,
        void *buf,
        size_t len,
        std::function<void(result<size_t>, inet4_address)> callback)
    {
        const auto fd = native_handle();

        loop.io().add(
            fd,
            io_event::readable,
            [this, &loop, buf, len, callback](socket_t cb_fd, io_event events)
            {
                if (!has_event(events, io_event::readable))
                    return;

                loop.io().remove(cb_fd);

                inet4_address src;
                auto r = recv_from(buf, len, src);

                if (!r)
                {
                    callback(r.error(), inet4_address {});
                    return;
                }

                callback(std::move(r), src);
            });
    }

    void udp6_socket::recv_async(
        event_loop &loop,
        void *buf,
        size_t len,
        std::function<void(result<size_t>, inet6_address)> callback)
    {
        const auto fd = native_handle();

        loop.io().add(
            fd,
            io_event::readable,
            [this, &loop, buf, len, callback](socket_t cb_fd, io_event events)
            {
                if (!has_event(events, io_event::readable))
                    return;

                loop.io().remove(cb_fd);

                inet6_address src;
                auto r = recv_from(buf, len, src);

                if (!r)
                {
                    callback(r.error(), inet6_address {});
                    return;
                }

                callback(std::move(r), src);
            });
    }

} // namespace socketpp

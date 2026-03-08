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

#ifndef SOCKETPP_FWD_HPP
#define SOCKETPP_FWD_HPP

#include <cstddef>
#include <cstdint>

namespace socketpp
{

    // ── Error ────────────────────────────────────────────────────────────────────

    enum class errc;
    template<typename T> class result;

    // ── Platform Types ───────────────────────────────────────────────────────────

    template<typename T> class span;
    struct iovec;
    struct msg_batch_entry;

    // ── Address ──────────────────────────────────────────────────────────────────

    class sock_address;
    class inet4_address;
    class inet6_address;

    // ── Socket ───────────────────────────────────────────────────────────────────

    class socket;
    class socket_options;
    struct buf_profile;
    class tcp_socket;
    class tcp4_socket;
    class tcp6_socket;
    class tcp_listener_base;
    class tcp4_listener;
    class tcp6_listener;
    struct tcp4_connector;
    struct tcp6_connector;
    class udp_socket_base;
    class udp4_socket;
    class udp6_socket;
    class udp4_peer_socket;
    class udp6_peer_socket;

    // ── Event ────────────────────────────────────────────────────────────────────

    enum class io_event : uint32_t;
    class dispatcher;
    class event_loop;
    class timer_handle;

    // ── High-Level API ──────────────────────────────────────────────────────────

    struct stream_listen_config;
    struct stream_connect_config;
    class stream4;
    class stream6;
    struct dgram_config;
    class dgram4;
    class dgram6;

} // namespace socketpp

#endif // SOCKETPP_FWD_HPP

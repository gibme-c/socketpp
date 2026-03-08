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

#ifndef SOCKETPP_HPP
#define SOCKETPP_HPP

// ── Platform ─────────────────────────────────────────────────────────────────

#include <socketpp/platform/capabilities.hpp>
#include <socketpp/platform/detect.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>
#include <socketpp/version.hpp>

// ── Network Addresses ────────────────────────────────────────────────────────

#include <socketpp/net/address.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>

// ── Sockets ──────────────────────────────────────────────────────────────────

#include <socketpp/socket/buf_profile.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>
#include <socketpp/socket/tcp.hpp>
#include <socketpp/socket/tcp_connector.hpp>
#include <socketpp/socket/tcp_listener.hpp>
#include <socketpp/socket/udp.hpp>
#include <socketpp/socket/udp_peer.hpp>

// ── Event Loop ───────────────────────────────────────────────────────────────

#include <socketpp/event/dispatcher.hpp>
#include <socketpp/event/loop.hpp>
#include <socketpp/event/timer.hpp>

// ── High-Level API ───────────────────────────────────────────────────────────

#include <socketpp/tcp_client.hpp>
#include <socketpp/tcp_connection.hpp>
#include <socketpp/tcp_server.hpp>
#include <socketpp/udp_server.hpp>

#endif // SOCKETPP_HPP

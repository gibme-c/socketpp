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

/// @file blackbox_verify.cpp
/// Compile-time verification that no platform types leak through public headers.
/// This file includes <socketpp.hpp> and static_asserts that platform types
/// are NOT visible. If this file compiles, the black-box boundary holds.

#include <socketpp/event/loop.hpp>
#include <socketpp/net/address.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/types.hpp>
#include <socketpp/socket/tcp.hpp>
#include <socketpp/socket/udp.hpp>

// Verify AF_INET is not defined (would come from winsock2.h or sys/socket.h)
#ifdef AF_INET
static_assert(false, "AF_INET is defined — platform headers are leaking");
#endif

#ifdef AF_INET6
static_assert(false, "AF_INET6 is defined — platform headers are leaking");
#endif

// Verify SOCK_STREAM is not defined
#ifdef SOCK_STREAM
static_assert(false, "SOCK_STREAM is defined — platform headers are leaking");
#endif

// Verify SOCK_DGRAM is not defined
#ifdef SOCK_DGRAM
static_assert(false, "SOCK_DGRAM is defined — platform headers are leaking");
#endif

// Verify SOMAXCONN is not defined
#ifdef SOMAXCONN
static_assert(false, "SOMAXCONN is defined — platform headers are leaking");
#endif

// Verify INVALID_SOCKET is not defined (Windows winsock2.h)
#ifdef INVALID_SOCKET
static_assert(false, "INVALID_SOCKET is defined — platform headers are leaking");
#endif

// Verify SOCKET_ERROR is not defined (Windows winsock2.h)
#ifdef SOCKET_ERROR
static_assert(false, "SOCKET_ERROR is defined — platform headers are leaking");
#endif

// Verify that only socketpp:: types are accessible
static_assert(sizeof(socketpp::socket_t) > 0, "socket_t should be accessible");
static_assert(sizeof(socketpp::inet4_address) > 0, "inet4_address should be accessible");
static_assert(sizeof(socketpp::inet6_address) > 0, "inet6_address should be accessible");
static_assert(sizeof(socketpp::sock_address) > 0, "sock_address should be accessible");
static_assert(sizeof(socketpp::tcp_socket) > 0, "tcp_socket should be accessible");
static_assert(sizeof(socketpp::tcp4_socket) > 0, "tcp4_socket should be accessible");
static_assert(sizeof(socketpp::tcp6_socket) > 0, "tcp6_socket should be accessible");
static_assert(sizeof(socketpp::udp4_socket) > 0, "udp4_socket should be accessible");
static_assert(sizeof(socketpp::udp6_socket) > 0, "udp6_socket should be accessible");
static_assert(sizeof(socketpp::event_loop) > 0, "event_loop should be accessible");

int main()
{
    return 0;
}

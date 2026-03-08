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

#ifndef SOCKETPP_SRC_PLATFORM_DETECT_INTERNAL_HPP
#define SOCKETPP_SRC_PLATFORM_DETECT_INTERNAL_HPP

#include <cstddef>
#include <socketpp/platform/detect.hpp>

// ── Platform Includes (private — never installed) ────────────────────────────

#if defined(SOCKETPP_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
// clang-format on
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#if defined(SOCKETPP_OS_LINUX)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#elif defined(SOCKETPP_OS_MACOS)
#include <sys/event.h>
#endif
#endif

// ── ssize_t shim for Windows (internal only) ─────────────────────────────────

#if defined(SOCKETPP_OS_WINDOWS)
namespace socketpp
{
    using ssize_t = std::ptrdiff_t;
} // namespace socketpp
#endif

// ── Static assertions for opaque storage sizes ───────────────────────────────

static_assert(sizeof(sockaddr_storage) <= 128, "sockaddr_storage exceeds opaque storage");
static_assert(sizeof(sockaddr_in) <= 16, "sockaddr_in exceeds opaque storage");
static_assert(sizeof(sockaddr_in6) <= 28, "sockaddr_in6 exceeds opaque storage");

#endif // SOCKETPP_SRC_PLATFORM_DETECT_INTERNAL_HPP

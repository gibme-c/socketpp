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

#ifndef SOCKETPP_PLATFORM_CAPABILITIES_HPP
#define SOCKETPP_PLATFORM_CAPABILITIES_HPP

#include <socketpp/platform/detect.hpp>

namespace socketpp
{

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
    inline constexpr bool has_reuseport = true;
#else
    inline constexpr bool has_reuseport = false;
#endif

#if defined(SOCKETPP_OS_LINUX)
    inline constexpr bool has_recvmmsg = true;
#else
    inline constexpr bool has_recvmmsg = false;
#endif

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS) || defined(SOCKETPP_OS_WINDOWS)
    inline constexpr bool has_tcp_fastopen = true;
#else
    inline constexpr bool has_tcp_fastopen = false;
#endif

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
    inline constexpr bool has_tcp_notsent_lowat = true;
#else
    inline constexpr bool has_tcp_notsent_lowat = false;
#endif

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
    inline constexpr bool has_tcp_cork = true;
#else
    inline constexpr bool has_tcp_cork = false;
#endif

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
    inline constexpr bool has_kernel_udp_demux = true;
#else
    inline constexpr bool has_kernel_udp_demux = false;
#endif

#if defined(SOCKETPP_OS_LINUX)
    inline constexpr bool has_tcp_user_timeout = true;
#else
    inline constexpr bool has_tcp_user_timeout = false;
#endif

#if defined(SOCKETPP_OS_LINUX)
    inline constexpr bool has_tcp_defer_accept = true;
#else
    inline constexpr bool has_tcp_defer_accept = false;
#endif

} // namespace socketpp

#endif // SOCKETPP_PLATFORM_CAPABILITIES_HPP

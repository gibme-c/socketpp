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

#include "detect_internal.hpp"

#include <socketpp/platform/error.hpp>

namespace socketpp
{

    std::error_code last_socket_error() noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        return {WSAGetLastError(), std::system_category()};
#else
        return {errno, std::system_category()};
#endif
    }

    std::error_code normalize_error(std::error_code ec) noexcept
    {
        if (!ec)
            return ec;

        if (ec.category() != std::system_category())
            return ec;

        const int code = ec.value();

#if defined(SOCKETPP_OS_WINDOWS)
        switch (code)
        {
            case WSAEWOULDBLOCK:
                return make_error_code(errc::would_block);
            case WSAEINPROGRESS:
                return make_error_code(errc::in_progress);
            case WSAECONNRESET:
                return make_error_code(errc::connection_reset);
            case WSAECONNREFUSED:
                return make_error_code(errc::connection_refused);
            case WSAECONNABORTED:
                return make_error_code(errc::connection_aborted);
            case WSAENOTCONN:
                return make_error_code(errc::not_connected);
            case WSAEADDRINUSE:
                return make_error_code(errc::address_in_use);
            case WSAENETUNREACH:
                return make_error_code(errc::network_unreachable);
            case WSAETIMEDOUT:
                return make_error_code(errc::timed_out);
            case WSAEINTR:
                return make_error_code(errc::interrupted);
            case WSAEMFILE:
                return make_error_code(errc::fd_limit_reached);
            default:
                return ec;
        }
#else
        switch (code)
        {
            case EAGAIN:
                return make_error_code(errc::would_block);
#if EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
                return make_error_code(errc::would_block);
#endif
            case EINPROGRESS:
                return make_error_code(errc::in_progress);
            case ECONNRESET:
                return make_error_code(errc::connection_reset);
            case ECONNREFUSED:
                return make_error_code(errc::connection_refused);
            case ECONNABORTED:
                return make_error_code(errc::connection_aborted);
            case ENOTCONN:
                return make_error_code(errc::not_connected);
            case EADDRINUSE:
                return make_error_code(errc::address_in_use);
            case ENETUNREACH:
                return make_error_code(errc::network_unreachable);
            case ETIMEDOUT:
                return make_error_code(errc::timed_out);
            case EINTR:
                return make_error_code(errc::interrupted);
            case EMFILE:
            case ENFILE:
                return make_error_code(errc::fd_limit_reached);
            default:
                return ec;
        }
#endif
    }

} // namespace socketpp

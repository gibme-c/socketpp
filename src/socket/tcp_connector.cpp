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

#include <socketpp/socket/tcp_connector.hpp>

namespace socketpp
{

    namespace
    {

        result<tcp_socket>
            connect_impl(address_family af, const sock_address &addr, const socket_options &opts) noexcept
        {
            auto r = socket::create(af, socket_type::stream);
            if (!r)
                return r.error();

            auto sock = std::move(r.value());

            auto apply_r = opts.apply_to(sock.native_handle());
            if (!apply_r)
            {
                sock.close();
                return apply_r.error();
            }

#if defined(SOCKETPP_OS_WINDOWS)
            auto rc = ::connect(
                static_cast<SOCKET>(sock.native_handle()),
                reinterpret_cast<const sockaddr *>(addr.data()),
                static_cast<socklen_t>(addr.size()));
#else
            auto rc = ::connect(
                static_cast<int>(sock.native_handle()),
                reinterpret_cast<const sockaddr *>(addr.data()),
                static_cast<socklen_t>(addr.size()));
#endif

            if (rc == 0)
                return tcp_socket(std::move(sock));

            auto ec = normalize_error(last_socket_error());

            // Remap would_block to in_progress after connect for consistency.
            if (ec == make_error_code(errc::would_block))
                ec = make_error_code(errc::in_progress);

            if (ec == make_error_code(errc::in_progress))
                return tcp_socket(std::move(sock));

            sock.close();
            return ec;
        }

    } // namespace

    result<tcp4_socket> tcp4_connector::connect(const inet4_address &addr, const socket_options &opts) noexcept
    {
        auto r = connect_impl(address_family::ipv4, addr, opts);
        if (!r)
            return r.error();
        return tcp4_socket(std::move(r.value()));
    }

    result<tcp6_socket> tcp6_connector::connect(const inet6_address &addr, const socket_options &opts) noexcept
    {
        auto r = connect_impl(address_family::ipv6, addr, opts);
        if (!r)
            return r.error();
        return tcp6_socket(std::move(r.value()));
    }

} // namespace socketpp

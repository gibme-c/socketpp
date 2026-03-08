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

#include <algorithm>
#include <cstring>
#include <socketpp/socket/udp_peer.hpp>

namespace socketpp
{

    // ── Helper: create a connected UDP socket ────────────────────────────────────

    namespace
    {

        result<socket> create_connected_udp(
            address_family af,
            const sock_address &local,
            const sock_address &peer,
            const socket_options &opts) noexcept
        {
            auto r = socket::create(af, socket_type::dgram);
            if (!r)
                return r.error();

            auto sock = std::move(r.value());

            // SO_REUSEADDR + SO_REUSEPORT allow multiple connected UDP sockets to share the
            // same local address:port. The kernel uses the full 4-tuple (src ip, src port,
            // dst ip, dst port) to route incoming datagrams to the correct socket.
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
            int opt_val = 1;
            if (::setsockopt(
                    static_cast<int>(sock.native_handle()),
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    reinterpret_cast<const char *>(&opt_val),
                    static_cast<socklen_t>(sizeof(opt_val)))
                != 0)
            {
                auto ec = normalize_error(last_socket_error());
                sock.close();
                return ec;
            }

            (void)::setsockopt(
                static_cast<int>(sock.native_handle()),
                SOL_SOCKET,
                SO_REUSEPORT,
                reinterpret_cast<const char *>(&opt_val),
                static_cast<socklen_t>(sizeof(opt_val)));
#endif

            auto apply_r = opts.apply_to(sock.native_handle());
            if (!apply_r)
            {
                sock.close();
                return apply_r.error();
            }

            auto bind_r = sock.bind(local);
            if (!bind_r)
            {
                sock.close();
                return bind_r.error();
            }

#if defined(SOCKETPP_OS_WINDOWS)
            auto rc = ::connect(
                static_cast<SOCKET>(sock.native_handle()),
                reinterpret_cast<const sockaddr *>(peer.data()),
                static_cast<socklen_t>(peer.size()));
#else
            auto rc = ::connect(
                static_cast<int>(sock.native_handle()),
                reinterpret_cast<const sockaddr *>(peer.data()),
                static_cast<socklen_t>(peer.size()));
#endif

            if (rc != 0)
            {
                auto ec = normalize_error(last_socket_error());
                sock.close();
                return ec;
            }

            return sock;
        }

    } // namespace

    // ── udp4_peer_socket::impl (POSIX: connected socket; Windows: mux) ──────────

#if defined(SOCKETPP_OS_WINDOWS)

    // On Windows, we use a userspace mux since there's no kernel 4-tuple demux.
    // The mux is deferred to Phase 9 — for now, use a simple connected socket.
    // This will be upgraded to the full mux implementation in Phase 9.

    struct udp4_peer_socket::impl
    {
        socket sock;
    };

    struct udp6_peer_socket::impl
    {
        socket sock;
    };

#else

    struct udp4_peer_socket::impl
    {
        socket sock;
    };

    struct udp6_peer_socket::impl
    {
        socket sock;
    };

#endif

    // ── udp4_peer_socket ─────────────────────────────────────────────────────────

    udp4_peer_socket::~udp4_peer_socket() noexcept = default;

    udp4_peer_socket::udp4_peer_socket(udp4_peer_socket &&other) noexcept:
        peer_(other.peer_), impl_(std::move(other.impl_))
    {
    }

    udp4_peer_socket &udp4_peer_socket::operator=(udp4_peer_socket &&other) noexcept
    {
        if (this != &other)
        {
            peer_ = other.peer_;
            impl_ = std::move(other.impl_);
        }

        return *this;
    }

    result<udp4_peer_socket> udp4_peer_socket::create(
        const inet4_address &local,
        const inet4_address &peer,
        const socket_options &opts) noexcept
    {
        const sock_address local_sa = local;
        const sock_address peer_sa = peer;

        auto r = create_connected_udp(address_family::ipv4, local_sa, peer_sa, opts);
        if (!r)
            return r.error();

        udp4_peer_socket ps;
        ps.peer_ = peer;
        ps.impl_ = std::make_unique<impl>();
        ps.impl_->sock = std::move(r.value());

        return ps;
    }

    result<size_t> udp4_peer_socket::send(const void *data, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::send(
            static_cast<SOCKET>(impl_->sock.native_handle()),
            static_cast<const char *>(data),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::send(static_cast<int>(impl_->sock.native_handle()), data, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    result<size_t> udp4_peer_socket::recv(void *buf, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::recv(
            static_cast<SOCKET>(impl_->sock.native_handle()),
            static_cast<char *>(buf),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::recv(static_cast<int>(impl_->sock.native_handle()), buf, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    socket_t udp4_peer_socket::native_handle() const noexcept
    {
        return impl_ ? impl_->sock.native_handle() : invalid_socket;
    }

    bool udp4_peer_socket::is_open() const noexcept
    {
        return impl_ && impl_->sock.is_open();
    }

    // ── udp6_peer_socket ─────────────────────────────────────────────────────────

    udp6_peer_socket::~udp6_peer_socket() noexcept = default;

    udp6_peer_socket::udp6_peer_socket(udp6_peer_socket &&other) noexcept:
        peer_(other.peer_), impl_(std::move(other.impl_))
    {
    }

    udp6_peer_socket &udp6_peer_socket::operator=(udp6_peer_socket &&other) noexcept
    {
        if (this != &other)
        {
            peer_ = other.peer_;
            impl_ = std::move(other.impl_);
        }

        return *this;
    }

    result<udp6_peer_socket> udp6_peer_socket::create(
        const inet6_address &local,
        const inet6_address &peer,
        const socket_options &opts) noexcept
    {
        const sock_address local_sa = local;
        const sock_address peer_sa = peer;

        auto r = create_connected_udp(address_family::ipv6, local_sa, peer_sa, opts);
        if (!r)
            return r.error();

        udp6_peer_socket ps;
        ps.peer_ = peer;
        ps.impl_ = std::make_unique<impl>();
        ps.impl_->sock = std::move(r.value());

        return ps;
    }

    result<size_t> udp6_peer_socket::send(const void *data, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::send(
            static_cast<SOCKET>(impl_->sock.native_handle()),
            static_cast<const char *>(data),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::send(static_cast<int>(impl_->sock.native_handle()), data, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    result<size_t> udp6_peer_socket::recv(void *buf, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::recv(
            static_cast<SOCKET>(impl_->sock.native_handle()),
            static_cast<char *>(buf),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::recv(static_cast<int>(impl_->sock.native_handle()), buf, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    socket_t udp6_peer_socket::native_handle() const noexcept
    {
        return impl_ ? impl_->sock.native_handle() : invalid_socket;
    }

    bool udp6_peer_socket::is_open() const noexcept
    {
        return impl_ && impl_->sock.is_open();
    }

} // namespace socketpp

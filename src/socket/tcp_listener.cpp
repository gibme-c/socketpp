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

#include <cstring>
#include <socketpp/socket/tcp_listener.hpp>

namespace socketpp
{

    // ── tcp_listener_base ────────────────────────────────────────────────────────

    result<void> tcp_listener_base::open(
        address_family af,
        const sock_address &bind_addr,
        int backlog,
        const socket_options &opts) noexcept
    {
        auto r = socket::create(af, socket_type::stream);

        if (!r)
            return r.error();

        socket_ = std::move(r.value());

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
        int opt_val = 1;

        if (::setsockopt(
                static_cast<int>(socket_.native_handle()),
                SOL_SOCKET,
                SO_REUSEADDR,
                reinterpret_cast<const char *>(&opt_val),
                static_cast<socklen_t>(sizeof(opt_val)))
            != 0)
        {
            auto ec = normalize_error(last_socket_error());
            socket_.close();
            return ec;
        }

        (void)::setsockopt(
            static_cast<int>(socket_.native_handle()),
            SOL_SOCKET,
            SO_REUSEPORT,
            reinterpret_cast<const char *>(&opt_val),
            static_cast<socklen_t>(sizeof(opt_val)));
#elif defined(SOCKETPP_OS_WINDOWS)
        if (!opts.has_reuse_addr())
        {
            int opt_val = 1;

            if (::setsockopt(
                    static_cast<SOCKET>(socket_.native_handle()),
                    SOL_SOCKET,
                    SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char *>(&opt_val),
                    static_cast<int>(sizeof(opt_val)))
                != 0)
            {
                auto ec = normalize_error(last_socket_error());
                socket_.close();
                return ec;
            }
        }
#endif

        auto apply_r = opts.apply_to(socket_.native_handle());

        if (!apply_r)
        {
            socket_.close();
            return apply_r.error();
        }

        auto bind_r = socket_.bind(bind_addr);

        if (!bind_r)
        {
            socket_.close();
            return bind_r.error();
        }

#if defined(SOCKETPP_OS_WINDOWS)
        if (::listen(static_cast<SOCKET>(socket_.native_handle()), backlog) != 0)
#else
        if (::listen(static_cast<int>(socket_.native_handle()), backlog) != 0)
#endif
        {
            auto ec = normalize_error(last_socket_error());
            socket_.close();
            return ec;
        }

        return {};
    }

    result<tcp_socket> tcp_listener_base::accept_raw() noexcept
    {
        if (SOCKETPP_UNLIKELY(!socket_.is_open()))
            return make_error_code(errc::invalid_state);

        sock_address peer_sa;
        auto peer_len = static_cast<socklen_t>(peer_sa.size());

#if defined(SOCKETPP_OS_LINUX)
        auto fd = ::accept4(
            static_cast<int>(socket_.native_handle()),
            reinterpret_cast<sockaddr *>(peer_sa.data()),
            &peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd < 0)
            return normalize_error(last_socket_error());

        tcp_socket conn(static_cast<socket_t>(fd));

#elif defined(SOCKETPP_OS_MACOS)
        auto fd = ::accept(
            static_cast<int>(socket_.native_handle()), reinterpret_cast<sockaddr *>(peer_sa.data()), &peer_len);

        if (fd < 0)
            return normalize_error(last_socket_error());

        tcp_socket conn(static_cast<socket_t>(fd));

        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            conn.close();
            return ec;
        }

        auto nb = conn.set_non_blocking(true);
        if (!nb)
        {
            conn.close();
            return nb.error();
        }

        int sigpipe_opt = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &sigpipe_opt, sizeof(sigpipe_opt)) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            conn.close();
            return ec;
        }

#elif defined(SOCKETPP_OS_WINDOWS)
        auto fd = ::accept(
            static_cast<SOCKET>(socket_.native_handle()), reinterpret_cast<sockaddr *>(peer_sa.data()), &peer_len);

        if (fd == INVALID_SOCKET)
            return normalize_error(last_socket_error());

        // Prevent the accepted socket handle from being inherited by child processes.
        ::SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);

        tcp_socket conn(static_cast<socket_t>(fd));

        auto nb = conn.set_non_blocking(true);
        if (!nb)
        {
            conn.close();
            return nb.error();
        }
#endif

        if (!accepted_opts_empty_)
        {
            auto ar = accepted_opts_.apply_to(conn.native_handle());
            if (!ar)
            {
                conn.close();
                return ar.error();
            }
        }

        return conn;
    }

    result<tcp_socket> tcp_listener_base::accept_raw(sock_address &peer_out) noexcept
    {
        if (SOCKETPP_UNLIKELY(!socket_.is_open()))
            return make_error_code(errc::invalid_state);

        sock_address peer_sa;
        auto peer_len = static_cast<socklen_t>(peer_sa.size());

#if defined(SOCKETPP_OS_LINUX)
        auto fd = ::accept4(
            static_cast<int>(socket_.native_handle()),
            reinterpret_cast<sockaddr *>(peer_sa.data()),
            &peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd < 0)
            return normalize_error(last_socket_error());

        tcp_socket conn(static_cast<socket_t>(fd));

#elif defined(SOCKETPP_OS_MACOS)
        auto fd = ::accept(
            static_cast<int>(socket_.native_handle()), reinterpret_cast<sockaddr *>(peer_sa.data()), &peer_len);

        if (fd < 0)
            return normalize_error(last_socket_error());

        tcp_socket conn(static_cast<socket_t>(fd));

        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            conn.close();
            return ec;
        }

        auto nb = conn.set_non_blocking(true);
        if (!nb)
        {
            conn.close();
            return nb.error();
        }

        int sigpipe_opt = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &sigpipe_opt, sizeof(sigpipe_opt)) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            conn.close();
            return ec;
        }

#elif defined(SOCKETPP_OS_WINDOWS)
        auto fd = ::accept(
            static_cast<SOCKET>(socket_.native_handle()), reinterpret_cast<sockaddr *>(peer_sa.data()), &peer_len);

        if (fd == INVALID_SOCKET)
            return normalize_error(last_socket_error());

        ::SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);

        tcp_socket conn(static_cast<socket_t>(fd));

        auto nb = conn.set_non_blocking(true);
        if (!nb)
        {
            conn.close();
            return nb.error();
        }
#endif

        if (!accepted_opts_empty_)
        {
            auto ar = accepted_opts_.apply_to(conn.native_handle());
            if (!ar)
            {
                conn.close();
                return ar.error();
            }
        }

        peer_sa.set_size(static_cast<uint32_t>(peer_len));
        peer_out = peer_sa;

        return conn;
    }

    // ── tcp4_listener ────────────────────────────────────────────────────────────

    result<void> tcp4_listener::open(const inet4_address &bind_addr, int backlog, const socket_options &opts) noexcept
    {
        return tcp_listener_base::open(address_family::ipv4, bind_addr, backlog, opts);
    }

    result<tcp4_socket> tcp4_listener::accept() noexcept
    {
        auto r = accept_raw();
        if (!r)
            return r.error();
        return tcp4_socket(std::move(r.value()));
    }

    result<tcp4_socket> tcp4_listener::accept(inet4_address &peer_out) noexcept
    {
        sock_address sa;
        auto r = accept_raw(sa);
        if (!r)
            return r.error();

        std::memcpy(&peer_out, sa.data(), sizeof(peer_out));
        return tcp4_socket(std::move(r.value()));
    }

    // ── tcp6_listener ────────────────────────────────────────────────────────────

    result<void> tcp6_listener::open(const inet6_address &bind_addr, int backlog, const socket_options &opts) noexcept
    {
        return tcp_listener_base::open(address_family::ipv6, bind_addr, backlog, opts);
    }

    result<tcp6_socket> tcp6_listener::accept() noexcept
    {
        auto r = accept_raw();
        if (!r)
            return r.error();
        return tcp6_socket(std::move(r.value()));
    }

    result<tcp6_socket> tcp6_listener::accept(inet6_address &peer_out) noexcept
    {
        sock_address sa;
        auto r = accept_raw(sa);
        if (!r)
            return r.error();

        std::memcpy(&peer_out, sa.data(), sizeof(peer_out));
        return tcp6_socket(std::move(r.value()));
    }

} // namespace socketpp

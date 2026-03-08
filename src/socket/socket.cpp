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
#include "../platform/wsa_init.hpp"

#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>

namespace socketpp
{

    namespace
    {

        int to_af(address_family af) noexcept
        {
            return af == address_family::ipv6 ? AF_INET6 : AF_INET;
        }

        int to_socktype(socket_type type) noexcept
        {
            return type == socket_type::dgram ? SOCK_DGRAM : SOCK_STREAM;
        }

        int to_shutdown(shutdown_mode how) noexcept
        {
#if defined(SOCKETPP_OS_WINDOWS)
            switch (how)
            {
                case shutdown_mode::read:
                    return SD_RECEIVE;
                case shutdown_mode::write:
                    return SD_SEND;
                case shutdown_mode::both:
                    return SD_BOTH;
            }
            return SD_BOTH;
#else
            switch (how)
            {
                case shutdown_mode::read:
                    return SHUT_RD;
                case shutdown_mode::write:
                    return SHUT_WR;
                case shutdown_mode::both:
                    return SHUT_RDWR;
            }
            return SHUT_RDWR;
#endif
        }

        // Platform-correct cast for passing socket handles to OS calls.
        // On Windows, socket handles are UINT_PTR (64-bit on x64); OS functions take SOCKET.
        // Using static_cast<int> would truncate the handle on 64-bit Windows.
        // On POSIX, socket handles are file descriptors (int).
#if defined(SOCKETPP_OS_WINDOWS)
        inline SOCKET as_native(socket_t fd) noexcept
        {
            return static_cast<SOCKET>(fd);
        }
#else
        inline int as_native(socket_t fd) noexcept
        {
            return static_cast<int>(fd);
        }
#endif

    } // namespace

    result<socket> socket::create(address_family af, socket_type type, int protocol) noexcept
    {
        const int domain = to_af(af);
        const int stype = to_socktype(type);

#if defined(SOCKETPP_OS_WINDOWS)
        detail::wsa_init::ensure();

        const auto fd = static_cast<socket_t>(
            ::WSASocketW(domain, stype, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));

        if (fd == invalid_socket)
            return normalize_error(last_socket_error());

        socket s(fd);

        auto nb = s.set_non_blocking(true);

        if (!nb)
        {
            s.close();
            return nb.error();
        }

        return s;

#elif defined(SOCKETPP_OS_LINUX)
        const auto fd = static_cast<socket_t>(::socket(domain, stype | SOCK_CLOEXEC | SOCK_NONBLOCK, protocol));

        if (fd == invalid_socket)
            return normalize_error(last_socket_error());

        return socket(fd);

#elif defined(SOCKETPP_OS_MACOS)
        const auto fd = static_cast<socket_t>(::socket(domain, stype, protocol));

        if (fd == invalid_socket)
            return normalize_error(last_socket_error());

        socket s(fd);

        if (::fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            s.close();
            return ec;
        }

        auto nb = s.set_non_blocking(true);

        if (!nb)
        {
            s.close();
            return nb.error();
        }

        int opt_val = 1;

        if (::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_NOSIGPIPE, &opt_val, sizeof(opt_val)) == -1)
        {
            auto ec = normalize_error(last_socket_error());
            s.close();
            return ec;
        }

        return s;
#endif
    }

    result<void> socket::close() noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return {};

        if (on_close_deregister_)
        {
            on_close_deregister_(handle_);
            on_close_deregister_ = nullptr;
        }

        dispatcher_ = nullptr;

        const auto fd = std::exchange(handle_, invalid_socket);

#if defined(SOCKETPP_OS_WINDOWS)
        if (SOCKETPP_UNLIKELY(::closesocket(static_cast<SOCKET>(fd)) == SOCKET_ERROR))
            return normalize_error(last_socket_error());
#else
        if (SOCKETPP_UNLIKELY(::close(static_cast<int>(fd)) == -1))
            return normalize_error(last_socket_error());
#endif

        return {};
    }

    result<void> socket::shutdown(shutdown_mode how) noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

        if (SOCKETPP_UNLIKELY(::shutdown(as_native(handle_), to_shutdown(how)) != 0))
            return normalize_error(last_socket_error());

        return {};
    }

    result<void> socket::bind(const sock_address &addr) noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

        // Port 0 (ephemeral) is allowed at this layer. The high-level API
        // (dgram/stream) applies SO_EXCLUSIVEADDRUSE and other safety measures.

        if (SOCKETPP_UNLIKELY(
                ::bind(
                    as_native(handle_),
                    reinterpret_cast<const sockaddr *>(addr.data()),
                    static_cast<socklen_t>(addr.size()))
                != 0))
            return normalize_error(last_socket_error());

        return {};
    }

    result<void> socket::set_non_blocking(bool enable) noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_WINDOWS)
        u_long mode = enable ? 1 : 0;

        if (SOCKETPP_UNLIKELY(::ioctlsocket(static_cast<SOCKET>(handle_), FIONBIO, &mode) == SOCKET_ERROR))
            return normalize_error(last_socket_error());
#else
        int flags = ::fcntl(static_cast<int>(handle_), F_GETFL, 0);

        if (SOCKETPP_UNLIKELY(flags == -1))
            return normalize_error(last_socket_error());

        flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);

        if (SOCKETPP_UNLIKELY(::fcntl(static_cast<int>(handle_), F_SETFL, flags) == -1))
            return normalize_error(last_socket_error());
#endif

        return {};
    }

    result<sock_address> socket::local_address() const noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

        sock_address addr;
        auto len = static_cast<socklen_t>(addr.size());

        if (SOCKETPP_UNLIKELY(::getsockname(as_native(handle_), reinterpret_cast<sockaddr *>(addr.data()), &len) != 0))
            return normalize_error(last_socket_error());

        addr.set_size(static_cast<uint32_t>(len));

        return addr;
    }

    result<sock_address> socket::peer_address() const noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

        sock_address addr;
        auto len = static_cast<socklen_t>(addr.size());

        if (SOCKETPP_UNLIKELY(::getpeername(as_native(handle_), reinterpret_cast<sockaddr *>(addr.data()), &len) != 0))
            return normalize_error(last_socket_error());

        addr.set_size(static_cast<uint32_t>(len));

        return addr;
    }

    result<void> socket::apply(const socket_options &opts) noexcept
    {
        if (SOCKETPP_UNLIKELY(handle_ == invalid_socket))
            return make_error_code(errc::invalid_state);

        auto r = opts.apply_to(handle_);

        if (!r)
            return r.error();

        return {};
    }

} // namespace socketpp

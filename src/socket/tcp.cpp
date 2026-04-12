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
#include <cstddef>
#include <cstring>
#include <socketpp/socket/tcp.hpp>
#include <vector>

#if !defined(SOCKETPP_OS_WINDOWS)
static_assert(sizeof(socketpp::iovec) == sizeof(struct ::iovec), "iovec size mismatch");
static_assert(offsetof(socketpp::iovec, iov_base) == offsetof(struct ::iovec, iov_base), "iov_base offset mismatch");
static_assert(offsetof(socketpp::iovec, iov_len) == offsetof(struct ::iovec, iov_len), "iov_len offset mismatch");
#endif

namespace socketpp
{

    // ── tcp_socket ───────────────────────────────────────────────────────────────

    result<size_t> tcp_socket::send(const void *data, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(send_lock_);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

#if defined(SOCKETPP_OS_WINDOWS)
        // Clamp len to INT_MAX: Windows send() takes int for the length parameter.
        // Buffers exceeding ~2.1GB are truncated to avoid silent wraparound.
        auto n = ::send(
            static_cast<SOCKET>(handle_),
            static_cast<const char *>(data),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::send(static_cast<int>(handle_), data, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    result<size_t> tcp_socket::recv(void *buf, size_t len, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(recv_lock_);

#if defined(SOCKETPP_OS_WINDOWS)
        // Clamp len to INT_MAX: Windows recv() takes int for the length parameter.
        auto n = ::recv(
            static_cast<SOCKET>(handle_),
            static_cast<char *>(buf),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags);
#else
        auto n = ::recv(static_cast<int>(handle_), buf, len, flags);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    result<size_t> tcp_socket::send_iov(span<const iovec> bufs)
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(send_lock_);

#if defined(SOCKETPP_OS_WINDOWS)
        constexpr size_t stack_limit = 16;
        WSABUF stack_bufs[stack_limit];
        std::vector<WSABUF> heap_bufs;
        WSABUF *wsa_bufs = stack_bufs;

        if (bufs.size() > stack_limit)
        {
            heap_bufs.resize(bufs.size());
            wsa_bufs = heap_bufs.data();
        }

        for (size_t i = 0; i < bufs.size(); ++i)
        {
            wsa_bufs[i].buf = static_cast<char *>(bufs[i].iov_base);
            // Clamp individual buffer lengths to ULONG_MAX to avoid truncation on 64-bit.
            wsa_bufs[i].len = static_cast<ULONG>((std::min)(bufs[i].iov_len, static_cast<size_t>(ULONG_MAX)));
        }

        DWORD bytes_sent = 0;

        auto rc = ::WSASend(
            static_cast<SOCKET>(handle_), wsa_bufs, static_cast<DWORD>(bufs.size()), &bytes_sent, 0, nullptr, nullptr);

        if (SOCKETPP_UNLIKELY(rc == SOCKET_ERROR))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(bytes_sent);

#elif defined(SOCKETPP_OS_LINUX)
        struct msghdr msg = {};
        msg.msg_iov = const_cast<struct ::iovec *>(reinterpret_cast<const struct ::iovec *>(bufs.data()));
        msg.msg_iovlen = bufs.size();

        auto n = ::sendmsg(static_cast<int>(handle_), &msg, MSG_NOSIGNAL);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);

#elif defined(SOCKETPP_OS_MACOS)
        auto n = ::writev(
            static_cast<int>(handle_),
            reinterpret_cast<const struct ::iovec *>(bufs.data()),
            static_cast<int>(bufs.size()));

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    result<size_t> tcp_socket::recv_iov(span<iovec> bufs)
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(recv_lock_);

#if defined(SOCKETPP_OS_WINDOWS)
        constexpr size_t stack_limit = 16;
        WSABUF stack_bufs[stack_limit];
        std::vector<WSABUF> heap_bufs;
        WSABUF *wsa_bufs = stack_bufs;

        if (bufs.size() > stack_limit)
        {
            heap_bufs.resize(bufs.size());
            wsa_bufs = heap_bufs.data();
        }

        for (size_t i = 0; i < bufs.size(); ++i)
        {
            wsa_bufs[i].buf = static_cast<char *>(bufs[i].iov_base);
            wsa_bufs[i].len = static_cast<ULONG>((std::min)(bufs[i].iov_len, static_cast<size_t>(ULONG_MAX)));
        }

        DWORD bytes_received = 0;
        DWORD flags = 0;

        auto rc = ::WSARecv(
            static_cast<SOCKET>(handle_),
            wsa_bufs,
            static_cast<DWORD>(bufs.size()),
            &bytes_received,
            &flags,
            nullptr,
            nullptr);

        if (SOCKETPP_UNLIKELY(rc == SOCKET_ERROR))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(bytes_received);
#else
        auto n = ::readv(
            static_cast<int>(handle_), reinterpret_cast<struct ::iovec *>(bufs.data()), static_cast<int>(bufs.size()));

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    // ── tcp4_socket typed accessors ──────────────────────────────────────────────

    result<inet4_address> tcp4_socket::local_addr() const noexcept
    {
        auto r = local_address();
        if (!r)
            return r.error();

        inet4_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

    result<inet4_address> tcp4_socket::peer_addr() const noexcept
    {
        auto r = peer_address();
        if (!r)
            return r.error();

        inet4_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

    // ── tcp6_socket typed accessors ──────────────────────────────────────────────

    result<inet6_address> tcp6_socket::local_addr() const noexcept
    {
        auto r = local_address();
        if (!r)
            return r.error();

        inet6_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

    result<inet6_address> tcp6_socket::peer_addr() const noexcept
    {
        auto r = peer_address();
        if (!r)
            return r.error();

        inet6_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

} // namespace socketpp

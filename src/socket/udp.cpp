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
#include <socketpp/socket/udp.hpp>
#include <vector>

namespace socketpp
{

    // ── udp_socket_base ──────────────────────────────────────────────────────────

    result<void>
        udp_socket_base::open(address_family af, const sock_address &bind_addr, const socket_options &opts) noexcept
    {
        auto r = socket::create(af, socket_type::dgram);

        if (!r)
            return r.error();

        *static_cast<socket *>(this) = std::move(r.value());

        auto apply_r = opts.apply_pre_bind(native_handle());

        if (!apply_r)
        {
            close();
            return apply_r.error();
        }

        auto bind_r = bind(bind_addr);

        if (!bind_r)
        {
            close();
            return bind_r.error();
        }

        auto post_r = opts.apply_post_bind(native_handle());

        if (!post_r)
        {
            close();
            return post_r.error();
        }

        return {};
    }

    result<size_t>
        udp_socket_base::send_to_raw(const void *data, size_t len, const sock_address &dest, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(send_lock_);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::sendto(
            static_cast<SOCKET>(handle_),
            static_cast<const char *>(data),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags,
            reinterpret_cast<const sockaddr *>(dest.data()),
            static_cast<int>(dest.size()));
#else
        auto n = ::sendto(
            static_cast<int>(handle_),
            data,
            len,
            flags,
            reinterpret_cast<const sockaddr *>(dest.data()),
            static_cast<socklen_t>(dest.size()));
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
    }

    result<size_t> udp_socket_base::recv_from_raw(void *buf, size_t len, sock_address &src_out, int flags) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

        spin_guard lock(recv_lock_);

        sock_address sa;
        auto sa_len = static_cast<socklen_t>(sa.capacity());

#if defined(SOCKETPP_OS_WINDOWS)
        auto n = ::recvfrom(
            static_cast<SOCKET>(handle_),
            static_cast<char *>(buf),
            static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
            flags,
            reinterpret_cast<sockaddr *>(sa.data()),
            &sa_len);
#else
        auto n =
            ::recvfrom(static_cast<int>(handle_), buf, len, flags, reinterpret_cast<sockaddr *>(sa.data()), &sa_len);
#endif

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        sa.set_size(static_cast<uint32_t>(sa_len));
        src_out = sa;

        return static_cast<size_t>(n);
    }

    result<size_t> udp_socket_base::send_to_iov_raw(span<const iovec> bufs, const sock_address &dest, int flags)
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
            wsa_bufs[i].len = static_cast<ULONG>(bufs[i].iov_len);
        }

        DWORD bytes_sent = 0;

        auto rc = ::WSASendTo(
            static_cast<SOCKET>(handle_),
            wsa_bufs,
            static_cast<DWORD>(bufs.size()),
            &bytes_sent,
            static_cast<DWORD>(flags),
            reinterpret_cast<const sockaddr *>(dest.data()),
            static_cast<int>(dest.size()),
            nullptr,
            nullptr);

        if (SOCKETPP_UNLIKELY(rc == SOCKET_ERROR))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(bytes_sent);

#elif defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;

        struct msghdr msg = {};
        msg.msg_name = const_cast<void *>(dest.data());
        msg.msg_namelen = static_cast<socklen_t>(dest.size());
        msg.msg_iov = const_cast<struct ::iovec *>(reinterpret_cast<const struct ::iovec *>(bufs.data()));
        msg.msg_iovlen = bufs.size();

        auto n = ::sendmsg(static_cast<int>(handle_), &msg, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);

#elif defined(SOCKETPP_OS_MACOS)
        struct msghdr msg = {};
        msg.msg_name = const_cast<void *>(dest.data());
        msg.msg_namelen = static_cast<socklen_t>(dest.size());
        msg.msg_iov = const_cast<struct ::iovec *>(reinterpret_cast<const struct ::iovec *>(bufs.data()));
        msg.msg_iovlen = static_cast<int>(bufs.size());

        auto n = ::sendmsg(static_cast<int>(handle_), &msg, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    result<size_t> udp_socket_base::recv_from_iov_raw(span<iovec> bufs, sock_address &src_out, int flags)
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
            wsa_bufs[i].len = static_cast<ULONG>(bufs[i].iov_len);
        }

        DWORD bytes_received = 0;
        DWORD wsa_flags = static_cast<DWORD>(flags);

        sock_address sa;
        auto sa_len = static_cast<INT>(sa.capacity());

        auto rc = ::WSARecvFrom(
            static_cast<SOCKET>(handle_),
            wsa_bufs,
            static_cast<DWORD>(bufs.size()),
            &bytes_received,
            &wsa_flags,
            reinterpret_cast<sockaddr *>(sa.data()),
            &sa_len,
            nullptr,
            nullptr);

        if (SOCKETPP_UNLIKELY(rc == SOCKET_ERROR))
            return normalize_error(last_socket_error());

        sa.set_size(static_cast<uint32_t>(sa_len));
        src_out = sa;

        return static_cast<size_t>(bytes_received);
#else
        sock_address sa;

        struct msghdr msg = {};
        msg.msg_name = sa.data();
        msg.msg_namelen = static_cast<socklen_t>(sa.capacity());
        msg.msg_iov = reinterpret_cast<struct ::iovec *>(bufs.data());
        msg.msg_iovlen = bufs.size();

        auto n = ::recvmsg(static_cast<int>(handle_), &msg, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        sa.set_size(static_cast<uint32_t>(msg.msg_namelen));
        src_out = sa;

        return static_cast<size_t>(n);
#endif
    }

    result<int> udp_socket_base::recv_batch(span<msg_batch_entry> msgs) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        constexpr size_t stack_limit = 64;
        struct mmsghdr stack_hdrs[stack_limit];
        struct ::iovec stack_iovs[stack_limit];
        std::vector<struct mmsghdr> heap_hdrs;
        std::vector<struct ::iovec> heap_iovs;
        struct mmsghdr *hdrs = stack_hdrs;
        struct ::iovec *iovs = stack_iovs;

        if (msgs.size() > stack_limit)
        {
            heap_hdrs.resize(msgs.size());
            heap_iovs.resize(msgs.size());
            hdrs = heap_hdrs.data();
            iovs = heap_iovs.data();
        }

        for (size_t i = 0; i < msgs.size(); ++i)
        {
            iovs[i].iov_base = msgs[i].buf;
            iovs[i].iov_len = msgs[i].len;

            std::memset(&hdrs[i], 0, sizeof(hdrs[i]));
            hdrs[i].msg_hdr.msg_name = msgs[i].addr.data();
            hdrs[i].msg_hdr.msg_namelen = static_cast<socklen_t>(msgs[i].addr.capacity());
            hdrs[i].msg_hdr.msg_iov = &iovs[i];
            hdrs[i].msg_hdr.msg_iovlen = 1;
        }

        auto n = ::recvmmsg(
            static_cast<int>(handle_), hdrs, static_cast<unsigned int>(msgs.size()), MSG_WAITFORONE, nullptr);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        for (int i = 0; i < n; ++i)
        {
            msgs[i].transferred = hdrs[i].msg_len;
            msgs[i].addr.set_size(static_cast<uint32_t>(hdrs[i].msg_hdr.msg_namelen));
        }

        return n;
#else
        // Fallback: single recv_from per message
        for (size_t i = 0; i < msgs.size(); ++i)
        {
            auto r = recv_from_raw(msgs[i].buf, msgs[i].len, msgs[i].addr);

            if (!r)
            {
                if (i == 0)
                    return r.error();

                return static_cast<int>(i);
            }

            msgs[i].transferred = r.value();
        }

        return static_cast<int>(msgs.size());
#endif
    }

    result<int> udp_socket_base::send_batch(span<msg_batch_entry> msgs) noexcept
    {
        if (SOCKETPP_UNLIKELY(!is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        constexpr size_t stack_limit = 64;
        struct mmsghdr stack_hdrs[stack_limit];
        struct ::iovec stack_iovs[stack_limit];
        std::vector<struct mmsghdr> heap_hdrs;
        std::vector<struct ::iovec> heap_iovs;
        struct mmsghdr *hdrs = stack_hdrs;
        struct ::iovec *iovs = stack_iovs;

        if (msgs.size() > stack_limit)
        {
            heap_hdrs.resize(msgs.size());
            heap_iovs.resize(msgs.size());
            hdrs = heap_hdrs.data();
            iovs = heap_iovs.data();
        }

        for (size_t i = 0; i < msgs.size(); ++i)
        {
            iovs[i].iov_base = msgs[i].buf;
            iovs[i].iov_len = msgs[i].len;

            std::memset(&hdrs[i], 0, sizeof(hdrs[i]));
            hdrs[i].msg_hdr.msg_name = msgs[i].addr.data();
            hdrs[i].msg_hdr.msg_namelen = static_cast<socklen_t>(msgs[i].addr.size());
            hdrs[i].msg_hdr.msg_iov = &iovs[i];
            hdrs[i].msg_hdr.msg_iovlen = 1;
        }

        auto n = ::sendmmsg(static_cast<int>(handle_), hdrs, static_cast<unsigned int>(msgs.size()), MSG_NOSIGNAL);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        for (int i = 0; i < n; ++i)
            msgs[i].transferred = hdrs[i].msg_len;

        return n;
#else
        // Fallback: single send_to per message
        for (size_t i = 0; i < msgs.size(); ++i)
        {
            auto r = send_to_raw(msgs[i].buf, msgs[i].len, msgs[i].addr);

            if (!r)
            {
                if (i == 0)
                    return r.error();

                return static_cast<int>(i);
            }

            msgs[i].transferred = r.value();
        }

        return static_cast<int>(msgs.size());
#endif
    }

    // ── udp4_socket ──────────────────────────────────────────────────────────────

    result<void> udp4_socket::open(const inet4_address &bind_addr, const socket_options &opts) noexcept
    {
        return udp_socket_base::open(address_family::ipv4, bind_addr, opts);
    }

    result<size_t> udp4_socket::send_to(const void *data, size_t len, const inet4_address &dest, int flags) noexcept
    {
        const sock_address sa = dest;
        return send_to_raw(data, len, sa, flags);
    }

    result<size_t> udp4_socket::recv_from(void *buf, size_t len, inet4_address &src_out, int flags) noexcept
    {
        sock_address sa;
        auto r = recv_from_raw(buf, len, sa, flags);
        if (!r)
            return r.error();

        std::memcpy(&src_out, sa.data(), sizeof(src_out));
        return r.value();
    }

    result<size_t> udp4_socket::send_to_iov(span<const iovec> bufs, const inet4_address &dest, int flags)
    {
        const sock_address sa = dest;
        return send_to_iov_raw(bufs, sa, flags);
    }

    result<size_t> udp4_socket::recv_from_iov(span<iovec> bufs, inet4_address &src_out, int flags)
    {
        sock_address sa;
        auto r = recv_from_iov_raw(bufs, sa, flags);
        if (!r)
            return r.error();

        std::memcpy(&src_out, sa.data(), sizeof(src_out));
        return r.value();
    }

    result<inet4_address> udp4_socket::local_addr() const noexcept
    {
        auto r = local_address();
        if (!r)
            return r.error();

        inet4_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

    // ── udp6_socket ──────────────────────────────────────────────────────────────

    result<void> udp6_socket::open(const inet6_address &bind_addr, const socket_options &opts) noexcept
    {
        return udp_socket_base::open(address_family::ipv6, bind_addr, opts);
    }

    result<size_t> udp6_socket::send_to(const void *data, size_t len, const inet6_address &dest, int flags) noexcept
    {
        const sock_address sa = dest;
        return send_to_raw(data, len, sa, flags);
    }

    result<size_t> udp6_socket::recv_from(void *buf, size_t len, inet6_address &src_out, int flags) noexcept
    {
        sock_address sa;
        auto r = recv_from_raw(buf, len, sa, flags);
        if (!r)
            return r.error();

        std::memcpy(&src_out, sa.data(), sizeof(src_out));
        return r.value();
    }

    result<size_t> udp6_socket::send_to_iov(span<const iovec> bufs, const inet6_address &dest, int flags)
    {
        const sock_address sa = dest;
        return send_to_iov_raw(bufs, sa, flags);
    }

    result<size_t> udp6_socket::recv_from_iov(span<iovec> bufs, inet6_address &src_out, int flags)
    {
        sock_address sa;
        auto r = recv_from_iov_raw(bufs, sa, flags);
        if (!r)
            return r.error();

        std::memcpy(&src_out, sa.data(), sizeof(src_out));
        return r.value();
    }

    result<inet6_address> udp6_socket::local_addr() const noexcept
    {
        auto r = local_address();
        if (!r)
            return r.error();

        inet6_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

} // namespace socketpp

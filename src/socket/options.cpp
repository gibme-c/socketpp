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
#include <socketpp/socket/options.hpp>

#if defined(SOCKETPP_OS_WINDOWS)
#include <mstcpip.h>

#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)

struct tcp_keepalive
{
    ULONG onoff;
    ULONG keepalivetime;
    ULONG keepaliveinterval;
};
#endif

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif
#endif

#if defined(SOCKETPP_OS_MACOS)
#ifndef TCP_KEEPALIVE
#define TCP_KEEPALIVE 0x10
#endif
#endif

namespace socketpp
{

    namespace
    {

        int read_int(const std::vector<uint8_t> &data) noexcept
        {
            int val = 0;

            if (data.size() >= sizeof(int))
                std::memcpy(&val, data.data(), sizeof(int));

            return val;
        }

        result<void> set_int_opt(socket_t handle, int level, int optname, int value) noexcept
        {
#if defined(SOCKETPP_OS_WINDOWS)
            auto sock = static_cast<SOCKET>(handle);
#else
            auto sock = static_cast<int>(handle);
#endif

            if (::setsockopt(
                    sock, level, optname, reinterpret_cast<const char *>(&value), static_cast<socklen_t>(sizeof(value)))
                != 0)
                return normalize_error(last_socket_error());

            return {};
        }

        result<void> set_bytes_opt(socket_t handle, int level, int optname, const void *data, size_t len) noexcept
        {
#if defined(SOCKETPP_OS_WINDOWS)
            auto sock = static_cast<SOCKET>(handle);
#else
            auto sock = static_cast<int>(handle);
#endif

            if (::setsockopt(sock, level, optname, reinterpret_cast<const char *>(data), static_cast<socklen_t>(len))
                != 0)
                return normalize_error(last_socket_error());

            return {};
        }

        result<void> apply_linger(socket_t handle, const std::vector<uint8_t> &data) noexcept
        {
            int vals[2] = {};

            if (data.size() >= sizeof(vals))
                std::memcpy(vals, data.data(), sizeof(vals));

            struct ::linger lg
            {
            };
            lg.l_onoff = static_cast<decltype(lg.l_onoff)>(vals[0]);
            lg.l_linger = static_cast<decltype(lg.l_linger)>(vals[1]);

            return set_bytes_opt(handle, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }

        result<void> apply_multicast(socket_t handle, socket_option_id id, const std::vector<uint8_t> &data) noexcept
        {
            constexpr size_t addr_block = 128 + sizeof(uint32_t);

            if (data.size() < addr_block * 2)
                return make_error_code(errc::invalid_state);

            const auto *p = data.data();

            const auto *group_sa = reinterpret_cast<const sockaddr *>(p);
            const auto *iface_sa = reinterpret_cast<const sockaddr *>(p + addr_block);

            const bool join = (id == socket_option_id::multicast_join);

            if (group_sa->sa_family == AF_INET)
            {
                const auto *g4 = reinterpret_cast<const sockaddr_in *>(group_sa);
                const auto *i4 = reinterpret_cast<const sockaddr_in *>(iface_sa);

                struct ip_mreq mreq
                {
                };
                std::memcpy(&mreq.imr_multiaddr, &g4->sin_addr, sizeof(mreq.imr_multiaddr));
                std::memcpy(&mreq.imr_interface, &i4->sin_addr, sizeof(mreq.imr_interface));

                return set_bytes_opt(
                    handle, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            }
            else if (group_sa->sa_family == AF_INET6)
            {
                const auto *g6 = reinterpret_cast<const sockaddr_in6 *>(group_sa);
                const auto *i6 = reinterpret_cast<const sockaddr_in6 *>(iface_sa);

                struct ipv6_mreq mreq6
                {
                };
                std::memcpy(&mreq6.ipv6mr_multiaddr, &g6->sin6_addr, sizeof(mreq6.ipv6mr_multiaddr));
                mreq6.ipv6mr_interface = i6->sin6_scope_id;

                return set_bytes_opt(
                    handle, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6));
            }

            return make_error_code(errc::invalid_state);
        }

        result<void> apply_tcp_defer_accept(socket_t handle, int seconds) noexcept
        {
#if defined(SOCKETPP_OS_LINUX)
            return set_int_opt(handle, IPPROTO_TCP, TCP_DEFER_ACCEPT, seconds);
#else
            (void)handle;
            (void)seconds;
            return make_error_code(errc::option_not_supported);
#endif
        }

#if defined(SOCKETPP_OS_WINDOWS)
        result<void> apply_windows_keepalive(
            socket_t handle,
            int idle_sec,
            bool has_idle,
            int interval_sec,
            bool has_interval) noexcept
        {
            struct tcp_keepalive ka
            {
            };
            ka.onoff = 1;
            ka.keepalivetime = static_cast<ULONG>(has_idle ? idle_sec * 1000 : 7200000);
            ka.keepaliveinterval = static_cast<ULONG>(has_interval ? interval_sec * 1000 : 1000);

            DWORD bytes_returned = 0;

            if (WSAIoctl(
                    static_cast<SOCKET>(handle),
                    SIO_KEEPALIVE_VALS,
                    &ka,
                    sizeof(ka),
                    nullptr,
                    0,
                    &bytes_returned,
                    nullptr,
                    nullptr)
                != 0)
                return normalize_error(last_socket_error());

            return {};
        }
#endif

        result<void> apply_multicast_if(socket_t handle, const std::vector<uint8_t> &data) noexcept
        {
            if (data.size() < 128 + sizeof(uint32_t))
                return make_error_code(errc::invalid_state);

            const auto *sa = reinterpret_cast<const sockaddr_in *>(data.data());
            struct in_addr addr;
            std::memcpy(&addr, &sa->sin_addr, sizeof(addr));

            return set_bytes_opt(handle, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr));
        }

        result<void> apply_multicast_if_v6(socket_t handle, const std::vector<uint8_t> &data) noexcept
        {
            if (data.size() < sizeof(unsigned int))
                return make_error_code(errc::invalid_state);

            unsigned int if_index = 0;
            std::memcpy(&if_index, data.data(), sizeof(if_index));

            return set_int_opt(handle, IPPROTO_IPV6, IPV6_MULTICAST_IF, static_cast<int>(if_index));
        }

        bool is_multicast_entry(socket_option_id id) noexcept
        {
            return id == socket_option_id::multicast_join || id == socket_option_id::multicast_leave;
        }

    } // namespace

    result<apply_result> socket_options::apply_to(socket_t handle) const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        bool win_has_idle = false, win_has_interval = false;
        int win_idle_sec = 0, win_interval_sec = 0;
#endif

        for (const auto &entry : entries_)
        {
            if (!entry.platform_available)
                return make_error_code(errc::option_not_supported);

            const int val = read_int(entry.data);
            result<void> rc;

            switch (entry.id)
            {
                case socket_option_id::reuse_addr:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_REUSEADDR, val);
                    break;

                case socket_option_id::reuse_port:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, SOL_SOCKET, SO_REUSEPORT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::exclusive_addr:
#if defined(SOCKETPP_OS_WINDOWS)
                    rc = set_int_opt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::recv_buf:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_RCVBUF, val);
                    break;

                case socket_option_id::send_buf:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_SNDBUF, val);
                    break;

                case socket_option_id::tcp_nodelay:
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NODELAY, val);
                    break;

                case socket_option_id::tcp_cork:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_CORK, val);
#elif defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NOPUSH, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_notsent_lowat:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NOTSENT_LOWAT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_user_timeout:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_USER_TIMEOUT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_fastopen:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_FASTOPEN, val);
#elif defined(SOCKETPP_OS_WINDOWS)
#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 15
#endif
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_FASTOPEN, 1);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_defer_accept:
                    rc = apply_tcp_defer_accept(handle, val);
                    break;

                case socket_option_id::keep_alive:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_KEEPALIVE, val);
                    break;

                case socket_option_id::keep_alive_idle:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPIDLE, val);
#elif defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPALIVE, val);
#elif defined(SOCKETPP_OS_WINDOWS)
                    win_has_idle = true;
                    win_idle_sec = val;
#endif
                    break;

                case socket_option_id::keep_alive_interval:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPINTVL, val);
#elif defined(SOCKETPP_OS_WINDOWS)
                    win_has_interval = true;
                    win_interval_sec = val;
#endif
                    break;

                case socket_option_id::keep_alive_count:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPCNT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::linger_opt:
                    rc = apply_linger(handle, entry.data);
                    break;

                case socket_option_id::ipv6_only:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_V6ONLY, val);
                    break;

                case socket_option_id::ip_tos:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_TOS, val);
                    break;

                case socket_option_id::broadcast:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_BROADCAST, val);
                    break;

                case socket_option_id::multicast_join:
                case socket_option_id::multicast_leave:
                    rc = apply_multicast(handle, entry.id, entry.data);
                    break;

                case socket_option_id::multicast_ttl:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_MULTICAST_TTL, val);
                    break;

                case socket_option_id::multicast_loop:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_MULTICAST_LOOP, val);
                    break;

                case socket_option_id::multicast_ttl_v6:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, val);
                    break;

                case socket_option_id::multicast_loop_v6:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, val);
                    break;

                case socket_option_id::multicast_if:
                    rc = apply_multicast_if(handle, entry.data);
                    break;

                case socket_option_id::multicast_if_v6:
                    rc = apply_multicast_if_v6(handle, entry.data);
                    break;
            }

            if (!rc)
                return rc.error();
        }

#if defined(SOCKETPP_OS_WINDOWS)
        if (win_has_idle || win_has_interval)
        {
            auto rc = apply_windows_keepalive(handle, win_idle_sec, win_has_idle, win_interval_sec, win_has_interval);
            if (!rc)
                return rc.error();
        }
#endif

        apply_result ar;

#if defined(SOCKETPP_OS_WINDOWS)
        auto sock = static_cast<SOCKET>(handle);
#else
        auto sock = static_cast<int>(handle);
#endif

        if (has_send_buf_)
        {
            int val = 0;
            socklen_t len = sizeof(val);

            if (::getsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&val), &len) == 0)
                ar.actual_send_buf = val;
        }

        if (has_recv_buf_)
        {
            int val = 0;
            socklen_t len = sizeof(val);

            if (::getsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&val), &len) == 0)
                ar.actual_recv_buf = val;
        }

        return ar;
    }

    result<apply_result> socket_options::apply_pre_bind(socket_t handle) const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        bool win_has_idle = false, win_has_interval = false;
        int win_idle_sec = 0, win_interval_sec = 0;
#endif

        for (const auto &entry : entries_)
        {
            if (is_multicast_entry(entry.id))
                continue;

            if (!entry.platform_available)
                return make_error_code(errc::option_not_supported);

            const int val = read_int(entry.data);
            result<void> rc;

            switch (entry.id)
            {
                case socket_option_id::reuse_addr:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_REUSEADDR, val);
                    break;

                case socket_option_id::reuse_port:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, SOL_SOCKET, SO_REUSEPORT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::exclusive_addr:
#if defined(SOCKETPP_OS_WINDOWS)
                    rc = set_int_opt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::recv_buf:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_RCVBUF, val);
                    break;

                case socket_option_id::send_buf:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_SNDBUF, val);
                    break;

                case socket_option_id::tcp_nodelay:
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NODELAY, val);
                    break;

                case socket_option_id::tcp_cork:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_CORK, val);
#elif defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NOPUSH, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_notsent_lowat:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_NOTSENT_LOWAT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_user_timeout:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_USER_TIMEOUT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_fastopen:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_FASTOPEN, val);
#elif defined(SOCKETPP_OS_WINDOWS)
#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 15
#endif
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_FASTOPEN, 1);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::tcp_defer_accept:
                    rc = apply_tcp_defer_accept(handle, val);
                    break;

                case socket_option_id::keep_alive:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_KEEPALIVE, val);
                    break;

                case socket_option_id::keep_alive_idle:
#if defined(SOCKETPP_OS_LINUX)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPIDLE, val);
#elif defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPALIVE, val);
#elif defined(SOCKETPP_OS_WINDOWS)
                    win_has_idle = true;
                    win_idle_sec = val;
#endif
                    break;

                case socket_option_id::keep_alive_interval:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPINTVL, val);
#elif defined(SOCKETPP_OS_WINDOWS)
                    win_has_interval = true;
                    win_interval_sec = val;
#endif
                    break;

                case socket_option_id::keep_alive_count:
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                    rc = set_int_opt(handle, IPPROTO_TCP, TCP_KEEPCNT, val);
#else
                    rc = make_error_code(errc::option_not_supported);
#endif
                    break;

                case socket_option_id::linger_opt:
                    rc = apply_linger(handle, entry.data);
                    break;

                case socket_option_id::ipv6_only:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_V6ONLY, val);
                    break;

                case socket_option_id::ip_tos:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_TOS, val);
                    break;

                case socket_option_id::broadcast:
                    rc = set_int_opt(handle, SOL_SOCKET, SO_BROADCAST, val);
                    break;

                case socket_option_id::multicast_join:
                case socket_option_id::multicast_leave:
                    break; // Handled by apply_post_bind

                case socket_option_id::multicast_ttl:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_MULTICAST_TTL, val);
                    break;

                case socket_option_id::multicast_loop:
                    rc = set_int_opt(handle, IPPROTO_IP, IP_MULTICAST_LOOP, val);
                    break;

                case socket_option_id::multicast_ttl_v6:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, val);
                    break;

                case socket_option_id::multicast_loop_v6:
                    rc = set_int_opt(handle, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, val);
                    break;

                case socket_option_id::multicast_if:
                    rc = apply_multicast_if(handle, entry.data);
                    break;

                case socket_option_id::multicast_if_v6:
                    rc = apply_multicast_if_v6(handle, entry.data);
                    break;
            }

            if (!rc)
                return rc.error();
        }

#if defined(SOCKETPP_OS_WINDOWS)
        if (win_has_idle || win_has_interval)
        {
            auto rc = apply_windows_keepalive(handle, win_idle_sec, win_has_idle, win_interval_sec, win_has_interval);
            if (!rc)
                return rc.error();
        }
#endif

        apply_result ar;

#if defined(SOCKETPP_OS_WINDOWS)
        auto sock = static_cast<SOCKET>(handle);
#else
        auto sock = static_cast<int>(handle);
#endif

        if (has_send_buf_)
        {
            int val = 0;
            socklen_t len = sizeof(val);

            if (::getsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&val), &len) == 0)
                ar.actual_send_buf = val;
        }

        if (has_recv_buf_)
        {
            int val = 0;
            socklen_t len = sizeof(val);

            if (::getsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&val), &len) == 0)
                ar.actual_recv_buf = val;
        }

        return ar;
    }

    result<void> socket_options::apply_post_bind(socket_t handle) const noexcept
    {
        for (const auto &entry : entries_)
        {
            if (!is_multicast_entry(entry.id))
                continue;

            if (!entry.platform_available)
                return make_error_code(errc::option_not_supported);

            auto rc = apply_multicast(handle, entry.id, entry.data);

            if (!rc)
                return rc.error();
        }

        return {};
    }

    result<void> socket_options::leave_all_multicast(socket_t handle) const noexcept
    {
        for (const auto &entry : entries_)
        {
            if (entry.id != socket_option_id::multicast_join)
                continue;

            auto rc = apply_multicast(handle, socket_option_id::multicast_leave, entry.data);

            if (!rc)
                return rc.error();
        }

        return {};
    }

} // namespace socketpp

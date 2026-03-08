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

#if defined(SOCKETPP_OS_WINDOWS)
#include "udp_mux.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#endif

namespace socketpp
{

    // ── Helper: create a connected UDP socket ────────────────────────────────────

#if !defined(SOCKETPP_OS_WINDOWS)

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

#endif // !SOCKETPP_OS_WINDOWS

    // ── udp4_peer_socket::impl (POSIX: connected socket; Windows: mux) ──────────

#if defined(SOCKETPP_OS_WINDOWS)

    struct udp4_peer_socket::impl
    {
        std::shared_ptr<detail::udp_mux<inet4_address>> mux;
        inet4_address peer_addr;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::deque<std::vector<char>> recv_queue;
        std::atomic<bool> closed {false};

        ~impl()
        {
            if (mux)
                mux->deregister_peer(peer_addr);

            closed.store(true, std::memory_order_relaxed);
            queue_cv.notify_all();
        }
    };

    struct udp6_peer_socket::impl
    {
        std::shared_ptr<detail::udp_mux<inet6_address>> mux;
        inet6_address peer_addr;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::deque<std::vector<char>> recv_queue;
        std::atomic<bool> closed {false};

        ~impl()
        {
            if (mux)
                mux->deregister_peer(peer_addr);

            closed.store(true, std::memory_order_relaxed);
            queue_cv.notify_all();
        }
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

    udp4_peer_socket::udp4_peer_socket() noexcept = default;
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
#if defined(SOCKETPP_OS_WINDOWS)
        auto mux = detail::udp_mux<inet4_address>::get_or_create(local, opts);

        if (!mux)
            return make_error_code(errc::invalid_state);

        udp4_peer_socket ps;
        ps.peer_ = peer;
        ps.impl_ = std::make_unique<impl>();
        ps.impl_->mux = std::move(mux);
        ps.impl_->peer_addr = peer;

        auto *raw_impl = ps.impl_.get();

        auto reg_r = ps.impl_->mux->register_peer(
            peer,
            [raw_impl](const void *data, size_t len)
            {
                std::lock_guard<std::mutex> lk(raw_impl->queue_mutex);
                raw_impl->recv_queue.emplace_back(
                    static_cast<const char *>(data), static_cast<const char *>(data) + len);
                raw_impl->queue_cv.notify_one();
            });

        if (!reg_r)
            return reg_r.error();

        return ps;
#else
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
#endif
    }

    result<size_t> udp4_peer_socket::send(const void *data, size_t len, int flags) noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        (void)flags;

        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->mux))
            return make_error_code(errc::invalid_state);

        return impl_->mux->send_to(data, len, impl_->peer_addr);
#else
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

        auto n = ::send(static_cast<int>(impl_->sock.native_handle()), data, len, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    result<size_t> udp4_peer_socket::recv(void *buf, size_t len, int flags) noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        (void)flags;

        if (SOCKETPP_UNLIKELY(!impl_))
            return make_error_code(errc::invalid_state);

        try
        {
            std::unique_lock<std::mutex> lk(impl_->queue_mutex);

            impl_->queue_cv.wait(
                lk, [this] { return !impl_->recv_queue.empty() || impl_->closed.load(std::memory_order_relaxed); });

            if (impl_->recv_queue.empty())
                return make_error_code(errc::invalid_state);

            auto &front = impl_->recv_queue.front();
            auto copy_len = (std::min)(len, front.size());
            std::memcpy(buf, front.data(), copy_len);
            impl_->recv_queue.pop_front();

            return copy_len;
        }
        catch (...)
        {
            return make_error_code(errc::invalid_state);
        }
#else
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

        auto n = ::recv(static_cast<int>(impl_->sock.native_handle()), buf, len, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    socket_t udp4_peer_socket::native_handle() const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        return (impl_ && impl_->mux) ? impl_->mux->native_handle() : invalid_socket;
#else
        return impl_ ? impl_->sock.native_handle() : invalid_socket;
#endif
    }

    bool udp4_peer_socket::is_open() const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        return impl_ && impl_->mux && !impl_->closed.load(std::memory_order_relaxed);
#else
        return impl_ && impl_->sock.is_open();
#endif
    }

    // ── udp6_peer_socket ─────────────────────────────────────────────────────────

    udp6_peer_socket::udp6_peer_socket() noexcept = default;
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
#if defined(SOCKETPP_OS_WINDOWS)
        auto mux = detail::udp_mux<inet6_address>::get_or_create(local, opts);

        if (!mux)
            return make_error_code(errc::invalid_state);

        udp6_peer_socket ps;
        ps.peer_ = peer;
        ps.impl_ = std::make_unique<impl>();
        ps.impl_->mux = std::move(mux);
        ps.impl_->peer_addr = peer;

        auto *raw_impl = ps.impl_.get();

        auto reg_r = ps.impl_->mux->register_peer(
            peer,
            [raw_impl](const void *data, size_t len)
            {
                std::lock_guard<std::mutex> lk(raw_impl->queue_mutex);
                raw_impl->recv_queue.emplace_back(
                    static_cast<const char *>(data), static_cast<const char *>(data) + len);
                raw_impl->queue_cv.notify_one();
            });

        if (!reg_r)
            return reg_r.error();

        return ps;
#else
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
#endif
    }

    result<size_t> udp6_peer_socket::send(const void *data, size_t len, int flags) noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        (void)flags;

        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->mux))
            return make_error_code(errc::invalid_state);

        return impl_->mux->send_to(data, len, impl_->peer_addr);
#else
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

#if defined(SOCKETPP_OS_LINUX)
        flags |= MSG_NOSIGNAL;
#endif

        auto n = ::send(static_cast<int>(impl_->sock.native_handle()), data, len, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    result<size_t> udp6_peer_socket::recv(void *buf, size_t len, int flags) noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        (void)flags;

        if (SOCKETPP_UNLIKELY(!impl_))
            return make_error_code(errc::invalid_state);

        try
        {
            std::unique_lock<std::mutex> lk(impl_->queue_mutex);

            impl_->queue_cv.wait(
                lk, [this] { return !impl_->recv_queue.empty() || impl_->closed.load(std::memory_order_relaxed); });

            if (impl_->recv_queue.empty())
                return make_error_code(errc::invalid_state);

            auto &front = impl_->recv_queue.front();
            auto copy_len = (std::min)(len, front.size());
            std::memcpy(buf, front.data(), copy_len);
            impl_->recv_queue.pop_front();

            return copy_len;
        }
        catch (...)
        {
            return make_error_code(errc::invalid_state);
        }
#else
        if (SOCKETPP_UNLIKELY(!impl_ || !impl_->sock.is_open()))
            return make_error_code(errc::invalid_state);

        auto n = ::recv(static_cast<int>(impl_->sock.native_handle()), buf, len, flags);

        if (SOCKETPP_UNLIKELY(n < 0))
            return normalize_error(last_socket_error());

        return static_cast<size_t>(n);
#endif
    }

    socket_t udp6_peer_socket::native_handle() const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        return (impl_ && impl_->mux) ? impl_->mux->native_handle() : invalid_socket;
#else
        return impl_ ? impl_->sock.native_handle() : invalid_socket;
#endif
    }

    bool udp6_peer_socket::is_open() const noexcept
    {
#if defined(SOCKETPP_OS_WINDOWS)
        return impl_ && impl_->mux && !impl_->closed.load(std::memory_order_relaxed);
#else
        return impl_ && impl_->sock.is_open();
#endif
    }

} // namespace socketpp

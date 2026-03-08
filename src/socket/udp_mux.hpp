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

/**
 * @file udp_mux.hpp
 * @brief Userspace UDP multiplexer for platforms without kernel 4-tuple demux (Windows).
 *
 * On Linux and macOS, multiple connected UDP sockets sharing a local port get
 * kernel-level 4-tuple demultiplexing. On Windows, this does not exist, so a
 * userspace multiplexer is needed to route incoming datagrams to the correct
 * peer handler based on the source address.
 *
 * The `udp_mux` owns a single unconnected UDP socket and maintains a map of
 * peer addresses to receive callbacks. When `on_readable()` is called (typically
 * from an event loop), it drains all available datagrams and dispatches them to
 * the registered peer callback, or to a catch-all handler for unknown peers.
 *
 * Thread safety:
 * - Peer registration/deregistration is protected by a spinlock.
 * - Send operations are protected by a separate spinlock.
 * - The `on_readable()` drain loop acquires the dispatch lock per-datagram.
 *
 * A global registry (keyed by local address) ensures only one mux exists per
 * local address, using `shared_ptr` / `weak_ptr` for automatic cleanup.
 *
 * @note This header is internal (under `src/`) and not part of the public API.
 * @note Only compiled on Windows (`SOCKETPP_OS_WINDOWS`).
 */

#ifndef SOCKETPP_SRC_SOCKET_UDP_MUX_HPP
#define SOCKETPP_SRC_SOCKET_UDP_MUX_HPP

#include <socketpp/platform/detect.hpp>

#if defined(SOCKETPP_OS_WINDOWS)

#include "../platform/detect_internal.hpp"
#include "../platform/spinlock.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace socketpp::detail
{

    /**
     * @brief Userspace UDP datagram multiplexer for a single local address.
     *
     * Routes incoming datagrams to registered peer callbacks based on the source
     * address. Manages a single underlying UDP socket shared by all registered peers.
     *
     * @tparam Addr Address type (`inet4_address` or `inet6_address`).
     */
    template<typename Addr> class udp_mux : public std::enable_shared_from_this<udp_mux<Addr>>
    {
      public:
        /// Callback type for datagrams from a registered peer.
        using recv_callback = std::function<void(const void *data, size_t len)>;

        /// Callback type for datagrams from unregistered (unknown) sources.
        using catch_all_callback = std::function<void(const void *data, size_t len, const Addr &src)>;

        /**
         * @brief Get or create a mux for the given local address.
         *
         * If a mux already exists for this address (in the global registry), the
         * existing instance is returned. Otherwise, a new socket is created, bound,
         * and registered.
         *
         * @param local_addr The local address to bind the mux socket to.
         * @param opts       Socket options to apply to the mux socket.
         * @return A shared pointer to the mux, or `nullptr` if socket creation failed.
         */
        static std::shared_ptr<udp_mux> get_or_create(const Addr &local_addr, const socket_options &opts = {})
        {
            scoped_lock<spinlock> lock(registry_mutex_);

            auto it = registry_.find(local_addr);

            if (it != registry_.end())
            {
                auto existing = it->second.lock();

                if (existing)
                    return existing;

                registry_.erase(it);
            }

            auto mux = std::shared_ptr<udp_mux>(new udp_mux());

            constexpr auto af = std::is_same_v<Addr, inet4_address> ? address_family::ipv4 : address_family::ipv6;

            auto r = socket::create(af, socket_type::dgram);

            if (!r)
                return nullptr;

            mux->socket_ = std::move(r.value());

            auto ar = opts.apply_to(mux->socket_.native_handle());

            if (!ar)
            {
                mux->socket_.close();
                return nullptr;
            }

            const sock_address bind_sa = local_addr;
            auto br = mux->socket_.bind(bind_sa);

            if (!br)
            {
                mux->socket_.close();
                return nullptr;
            }

            mux->local_addr_ = local_addr;
            registry_.emplace(local_addr, mux);

            mux->poll_thread_ = std::thread([mux]() { mux->poll_loop(); });

            return mux;
        }

        ~udp_mux()
        {
            stop_requested_.store(true, std::memory_order_relaxed);

            if (poll_thread_.joinable())
                poll_thread_.join();

            scoped_lock<spinlock> lock(registry_mutex_);

            auto it = registry_.find(local_addr_);

            if (it != registry_.end() && it->second.expired())
                registry_.erase(it);
        }

        udp_mux(const udp_mux &) = delete;
        udp_mux &operator=(const udp_mux &) = delete;
        udp_mux(udp_mux &&) = delete;
        udp_mux &operator=(udp_mux &&) = delete;

        /**
         * @brief Register a peer address with a receive callback.
         *
         * @param peer The peer address to register.
         * @param cb   Callback invoked when a datagram from this peer arrives.
         * @return Success, or `errc::already_bound` if the peer is already registered.
         */
        result<void> register_peer(const Addr &peer, recv_callback cb)
        {
            scoped_lock<spinlock> lock(mutex_);

            auto it = peers_.find(peer);

            if (it != peers_.end())
                return make_error_code(errc::already_bound);

            peer_entry entry;
            entry.peer_addr = peer;
            entry.callback = std::move(cb);

            peers_.emplace(peer, std::move(entry));

            return {};
        }

        /** @brief Remove a previously registered peer. Safe to call if not registered. */
        void deregister_peer(const Addr &peer)
        {
            scoped_lock<spinlock> lock(mutex_);

            peers_.erase(peer);
        }

        /**
         * @brief Send a datagram to the specified destination via the shared socket.
         *
         * Thread-safe; protected by a dedicated send spinlock separate from the
         * dispatch spinlock to avoid contention between send and receive paths.
         *
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @param dest Destination address.
         * @return Number of bytes sent, or an error code.
         */
        result<size_t> send_to(const void *data, size_t len, const Addr &dest)
        {
            if (SOCKETPP_UNLIKELY(!socket_.is_open()))
                return make_error_code(errc::invalid_state);

            scoped_lock<spinlock> lock(send_mutex_);

            const sock_address sa = dest;

            auto n = ::sendto(
                static_cast<SOCKET>(socket_.native_handle()),
                static_cast<const char *>(data),
                static_cast<int>((std::min)(len, static_cast<size_t>(INT_MAX))),
                0,
                reinterpret_cast<const sockaddr *>(sa.data()),
                static_cast<int>(sa.size()));

            if (SOCKETPP_UNLIKELY(n < 0))
                return normalize_error(last_socket_error());

            return static_cast<size_t>(n);
        }

        /**
         * @brief Set a catch-all callback for datagrams from unregistered peers.
         * @param cb The callback, or `nullptr` to clear.
         */
        void set_catch_all(catch_all_callback cb)
        {
            scoped_lock<spinlock> lock(mutex_);

            catch_all_ = std::move(cb);
        }

        /** @brief Get the native handle of the shared socket. */
        socket_t native_handle() const noexcept
        {
            return socket_.native_handle();
        }

        /**
         * @brief Drain all available datagrams and dispatch to registered handlers.
         *
         * Should be called when the mux socket becomes readable (from an event loop
         * callback). Reads datagrams in a loop until `recvfrom()` returns an error
         * (typically `WSAEWOULDBLOCK`), then returns. Zero-length datagrams are
         * silently skipped.
         *
         * Uses a member buffer (`recv_buf_`) instead of stack allocation to avoid
         * 64 KB stack frames on each call.
         */
        void on_readable()
        {
            for (;;)
            {
                sock_address src_sa;
                auto src_len = static_cast<socklen_t>(src_sa.capacity());

                auto n = ::recvfrom(
                    static_cast<SOCKET>(socket_.native_handle()),
                    recv_buf_,
                    sizeof(recv_buf_),
                    0,
                    reinterpret_cast<sockaddr *>(src_sa.data()),
                    &src_len);

                if (n < 0)
                    break;

                if (n == 0)
                    continue;

                src_sa.set_size(static_cast<uint32_t>(src_len));

                Addr src;
                std::memcpy(&src, src_sa.data(), sizeof(src));

                scoped_lock<spinlock> lock(mutex_);

                auto it = peers_.find(src);

                if (it != peers_.end())
                {
                    it->second.callback(recv_buf_, static_cast<size_t>(n));
                }
                else if (catch_all_)
                {
                    catch_all_(recv_buf_, static_cast<size_t>(n), src);
                }
            }
        }

      private:
        udp_mux() = default;

        void poll_loop()
        {
            WSAPOLLFD pfd {};
            pfd.fd = static_cast<SOCKET>(socket_.native_handle());
            pfd.events = POLLIN;

            while (!stop_requested_.load(std::memory_order_relaxed))
            {
                auto rc = ::WSAPoll(&pfd, 1, 50);

                if (rc > 0 && (pfd.revents & POLLIN))
                    on_readable();
            }
        }

        socket socket_;
        Addr local_addr_;

        struct peer_entry
        {
            Addr peer_addr;
            recv_callback callback;
        };

        std::unordered_map<Addr, peer_entry> peers_;
        catch_all_callback catch_all_;
        spinlock mutex_; ///< Protects peers_ and catch_all_.
        spinlock send_mutex_; ///< Protects send_to() from concurrent access.

        // 64 KB member buffer for receiving datagrams. Allocated as part of the
        // mux object to avoid large stack frames on each on_readable() call.
        char recv_buf_[65536];

        std::thread poll_thread_;
        std::atomic<bool> stop_requested_ {false};

        // ── Global Registry ──────────────────────────────────────────────────

        static spinlock registry_mutex_;
        static std::unordered_map<Addr, std::weak_ptr<udp_mux>> registry_;
    };

    template<typename Addr> spinlock udp_mux<Addr>::registry_mutex_;

    template<typename Addr> std::unordered_map<Addr, std::weak_ptr<udp_mux<Addr>>> udp_mux<Addr>::registry_;

} // namespace socketpp::detail

#endif // SOCKETPP_OS_WINDOWS

#endif // SOCKETPP_SRC_SOCKET_UDP_MUX_HPP

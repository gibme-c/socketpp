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
 * @file dgram_peer_impl.hpp
 * @brief Internal implementation template for per-peer UDP handles.
 *
 * Platform strategy:
 * - Linux/macOS (has_kernel_udp_demux): Creates a connected UDP peer socket.
 *   The kernel routes matching 4-tuple traffic to this socket automatically.
 *   The peer socket's fd is registered with the parent's event loop.
 * - Windows (!has_kernel_udp_demux): No peer socket is created. The parent's
 *   handle_readable() checks claimed_peers_ and forwards matching datagrams
 *   via deliver_data(). Sends go through the parent's socket via send_to().
 *
 * This is an internal header -- not part of the public API.
 */

#ifndef SOCKETPP_DETAIL_DGRAM_PEER_IMPL_HPP
#define SOCKETPP_DETAIL_DGRAM_PEER_IMPL_HPP

#include "../platform/detect_internal.hpp"
#include "serial_queue.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <socketpp/dgram_peer.hpp>
#include <socketpp/event/loop.hpp>
#include <socketpp/platform/capabilities.hpp>
#include <socketpp/socket/udp.hpp>
#include <socketpp/socket/udp_peer.hpp>
#include <vector>

namespace socketpp::detail
{

    /**
     * @brief Implementation of a claimed peer handle.
     *
     * @tparam PeerType    Public type (dgram4_peer or dgram6_peer).
     * @tparam Address     Address type (inet4_address or inet6_address).
     * @tparam Socket      Parent socket type (udp4_socket or udp6_socket).
     * @tparam PeerSocket  Kernel-demux peer socket type (udp4_peer_socket or udp6_peer_socket).
     * @tparam SendEntry   Batch send entry type (dgram4_send_entry or dgram6_send_entry).
     */
    template<typename PeerType, typename Address, typename Socket, typename PeerSocket, typename SendEntry>
    struct dgram_peer_impl
    {
        Address peer_addr_;
        event_loop *loop_;
        thread_pool *pool_;
        Socket *parent_socket_;
        serial_queue serial_;

        typename PeerType::data_handler on_data_cb;
        typename PeerType::error_handler on_error_cb;

        std::atomic<bool> open_ {true};

        // Linux/macOS only: connected peer socket + read buffer
        PeerSocket peer_socket_;
        std::vector<char> read_buf_;

        dgram_peer_impl(const Address &peer, event_loop *loop, thread_pool *pool, Socket *parent_socket):
            peer_addr_(peer), loop_(loop), pool_(pool), parent_socket_(parent_socket), serial_(pool)
        {
        }

        /**
         * @brief Deliver data to this peer (called from parent's handle_readable on Windows,
         *        or from own handle_readable on Linux/macOS).
         *
         * Copies data and dispatches through the serial queue.
         */
        void deliver_data(const char *data, size_t len)
        {
            if (!open_.load(std::memory_order_relaxed) || !on_data_cb)
                return;

            auto buf = std::vector<char>(data, data + len);
            serial_.submit([this, d = std::move(buf)]() { on_data_cb(d.data(), d.size()); });
        }

        /**
         * @brief Register the peer socket for readable events (Linux/macOS only).
         *
         * Called when on_data() is set on the peer handle. On Windows this is a no-op
         * because data is forwarded from the parent's handle_readable().
         */
        void arm(size_t buf_size)
        {
            if constexpr (has_kernel_udp_demux)
            {
                if (!peer_socket_.is_open())
                    return;

                read_buf_.resize(buf_size);

                auto fd = peer_socket_.native_handle();

                loop_->post(
                    [this, fd]()
                    {
                        loop_->io().add(
                            fd,
                            io_event::readable,
                            [this](socket_t, io_event events)
                            {
                                if (!has_event(events, io_event::readable))
                                    return;

                                handle_readable();
                            });
                    });
            }
            // On Windows: no-op. Data is delivered by parent's handle_readable().
        }

        /**
         * @brief Recv loop for the connected peer socket (Linux/macOS only).
         */
        void handle_readable()
        {
            if constexpr (has_kernel_udp_demux)
            {
                for (;;)
                {
                    auto r = peer_socket_.recv(read_buf_.data(), read_buf_.size());

                    if (!r)
                    {
                        if (r.error() == make_error_code(errc::would_block))
                            break;

                        if (on_error_cb)
                        {
                            auto ec = r.error();
                            serial_.submit([this, ec]() { on_error_cb(ec); });
                        }

                        break;
                    }

                    if (r.value() == 0)
                        break;

                    deliver_data(read_buf_.data(), r.value());
                }
            }
        }

        /**
         * @brief Send data to the claimed peer.
         *
         * On Linux/macOS: uses the connected peer socket's send().
         * On Windows: uses the parent socket's send_to(). This is safe because
         * udp_socket_base::send_to_raw() holds a send_lock_ (spinlock) internally.
         */
        bool do_send(const void *data, size_t len)
        {
            if (!open_.load(std::memory_order_relaxed))
                return false;

            if constexpr (has_kernel_udp_demux)
            {
                auto r = peer_socket_.send(data, len);
                return !!r;
            }
            else
            {
                auto r = parent_socket_->send_to(data, len, peer_addr_);
                return !!r;
            }
        }

        /**
         * @brief Batch send to the claimed peer.
         *
         * Fills in the peer address for each entry and delegates to the parent
         * socket's send_batch(). On all platforms this goes through the parent
         * socket since batch send with per-entry addresses is more efficient
         * than N individual sends on the connected socket.
         */
        result<int> do_send_batch(span<const SendEntry> msgs)
        {
            if (!open_.load(std::memory_order_relaxed))
                return errc::not_connected;

            constexpr size_t stack_limit = 64;
            msg_batch_entry stack_buf[stack_limit];
            std::vector<msg_batch_entry> heap_buf;

            msg_batch_entry *entries;

            if (msgs.size() <= stack_limit)
            {
                entries = stack_buf;
            }
            else
            {
                heap_buf.resize(msgs.size());
                entries = heap_buf.data();
            }

            for (size_t i = 0; i < msgs.size(); ++i)
            {
                sock_address sa = peer_addr_;
                entries[i].buf = const_cast<void *>(msgs[i].data);
                entries[i].len = msgs[i].len;
                entries[i].addr = sa;
                entries[i].transferred = 0;
            }

            return parent_socket_->send_batch(span<msg_batch_entry>(entries, msgs.size()));
        }

        timer_handle do_defer(std::chrono::milliseconds delay, std::function<void()> cb)
        {
            auto p = std::make_shared<std::promise<timer_handle>>();
            auto f = p->get_future();
            auto *sq = &serial_;

            loop_->post(
                [this, delay, cb = std::move(cb), p, sq]() mutable
                {
                    auto h = loop_->defer(delay, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                    p->set_value(std::move(h));
                });

            return f.get();
        }

        timer_handle do_repeat(std::chrono::milliseconds interval, std::function<void()> cb)
        {
            auto p = std::make_shared<std::promise<timer_handle>>();
            auto f = p->get_future();
            auto *sq = &serial_;

            loop_->post(
                [this, interval, cb = std::move(cb), p, sq]() mutable
                {
                    auto h = loop_->repeat(interval, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                    p->set_value(std::move(h));
                });

            return f.get();
        }

        void do_post(std::function<void()> cb)
        {
            auto *sq = &serial_;
            loop_->post([sq, cb = std::move(cb)]() { sq->submit(cb); });
        }

        /**
         * @brief Clean up this peer. Called by relinquish() or from parent's do_destroy().
         *
         * On Linux/macOS: removes the peer socket fd from the event loop and closes it.
         * On Windows: no socket cleanup needed (data routing handled by parent).
         */
        void do_relinquish()
        {
            if (!open_.exchange(false, std::memory_order_acq_rel))
                return;

            if constexpr (has_kernel_udp_demux)
            {
                if (peer_socket_.is_open())
                {
                    loop_->io().remove(peer_socket_.native_handle());
                    // Close the socket now so the kernel stops routing the 4-tuple
                    // here. Without this, the connected socket stays open (impl is
                    // still alive in claimed_peers) and swallows datagrams that
                    // should fall through to the parent socket.
                    peer_socket_ = PeerSocket {};
                }
            }
        }
    };

} // namespace socketpp::detail

// ── Concrete impl structs ───────────────────────────────────────────────────
// Defined here (rather than in dgram_peer.cpp) so that both dgram.cpp and
// dgram_peer.cpp see the full type. dgram.cpp needs visibility because
// dgram4::claim() / dgram6::claim() construct these as friends.

namespace socketpp
{
    struct dgram4_peer::impl :
        detail::dgram_peer_impl<dgram4_peer, inet4_address, udp4_socket, udp4_peer_socket, dgram4_send_entry>
    {
        using dgram_peer_impl::dgram_peer_impl;
    };

    struct dgram6_peer::impl :
        detail::dgram_peer_impl<dgram6_peer, inet6_address, udp6_socket, udp6_peer_socket, dgram6_send_entry>
    {
        using dgram_peer_impl::dgram_peer_impl;
    };
} // namespace socketpp

#endif // SOCKETPP_DETAIL_DGRAM_PEER_IMPL_HPP

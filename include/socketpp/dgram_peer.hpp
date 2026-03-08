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
 * @file dgram_peer.hpp
 * @brief Per-peer UDP handles for claimed datagram traffic.
 *
 * dgram4_peer and dgram6_peer are obtained via dgram4::claim() / dgram6::claim().
 * They capture traffic from a specific peer address, isolating it from the parent
 * dgram's on_data callback. Each peer handle has its own serialized execution queue
 * guaranteeing at-most-one callback at a time.
 *
 * Lifecycle:
 * - claim() returns a peer handle; traffic from that peer is routed to the handle
 * - relinquish() or destructor returns traffic to the parent's on_data
 * - send()/send_batch() transmit to the claimed peer address
 * - defer()/repeat()/post() schedule callbacks through the parent's event loop
 *
 * Platform strategy:
 * - Linux/macOS: kernel 4-tuple demux via connected UDP socket
 * - Windows: in-process routing in the parent's handle_readable()
 */

#ifndef SOCKETPP_DGRAM_PEER_HPP
#define SOCKETPP_DGRAM_PEER_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <socketpp/event/timer.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>

namespace socketpp
{

    // Forward declarations
    class dgram4;
    class dgram6;
    struct dgram4_send_entry;
    struct dgram6_send_entry;

    /**
     * @brief Per-peer IPv4 UDP handle obtained via dgram4::claim().
     *
     * Captures all traffic from a specific peer address. Non-copyable, move-only.
     * RAII: destructor calls relinquish() if not already called.
     */
    class dgram4_peer
    {
      public:
        using data_handler = std::function<void(const char *, size_t)>;
        using error_handler = std::function<void(std::error_code)>;

        dgram4_peer &on_data(data_handler handler);
        dgram4_peer &on_error(error_handler handler);

        bool send(const void *data, size_t len);
        result<int> send_batch(span<const dgram4_send_entry> msgs);

        void relinquish();
        inet4_address peer_addr() const;
        bool is_open() const noexcept;

        timer_handle defer(std::chrono::milliseconds delay, std::function<void()> cb);
        timer_handle repeat(std::chrono::milliseconds interval, std::function<void()> cb);
        void post(std::function<void()> cb);

        ~dgram4_peer();
        dgram4_peer(dgram4_peer &&) noexcept;
        dgram4_peer &operator=(dgram4_peer &&) noexcept;
        dgram4_peer(const dgram4_peer &) = delete;
        dgram4_peer &operator=(const dgram4_peer &) = delete;

      private:
        friend class dgram4;
        dgram4_peer();
        struct impl;
        std::shared_ptr<impl> impl_;
    };

    /**
     * @brief Per-peer IPv6 UDP handle obtained via dgram6::claim().
     *
     * IPv6 counterpart to dgram4_peer. All semantics are identical.
     * @see dgram4_peer
     */
    class dgram6_peer
    {
      public:
        using data_handler = std::function<void(const char *, size_t)>;
        using error_handler = std::function<void(std::error_code)>;

        dgram6_peer &on_data(data_handler handler);
        dgram6_peer &on_error(error_handler handler);

        bool send(const void *data, size_t len);
        result<int> send_batch(span<const dgram6_send_entry> msgs);

        void relinquish();
        inet6_address peer_addr() const;
        bool is_open() const noexcept;

        timer_handle defer(std::chrono::milliseconds delay, std::function<void()> cb);
        timer_handle repeat(std::chrono::milliseconds interval, std::function<void()> cb);
        void post(std::function<void()> cb);

        ~dgram6_peer();
        dgram6_peer(dgram6_peer &&) noexcept;
        dgram6_peer &operator=(dgram6_peer &&) noexcept;
        dgram6_peer(const dgram6_peer &) = delete;
        dgram6_peer &operator=(const dgram6_peer &) = delete;

      private:
        friend class dgram6;
        dgram6_peer();
        struct impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_DGRAM_PEER_HPP

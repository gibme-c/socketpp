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
 * @file stream_connection_impl.hpp
 * @brief Shared internal implementation template for stream connections.
 *
 * Extends tcp_conn_impl with pause()/resume() flow control for connections.
 * Defines stream4::connection::impl and stream6::connection::impl.
 *
 * This is an internal header -- not part of the public API.
 */

#ifndef SOCKETPP_DETAIL_STREAM_CONNECTION_IMPL_HPP
#define SOCKETPP_DETAIL_STREAM_CONNECTION_IMPL_HPP

#include "connection_engine.hpp"

#include <socketpp/stream.hpp>

namespace socketpp::detail
{

    /**
     * @brief Connection implementation with pause/resume support.
     *
     * Inherits all connection logic from tcp_conn_impl and adds
     * pause_from_user() / resume_from_user() for flow control.
     */
    template<typename Socket, typename Address> struct stream_conn_impl : tcp_conn_impl<Socket, Address>
    {
        using tcp_conn_impl<Socket, Address>::tcp_conn_impl;

        std::atomic<bool> paused_ {false};

        /**
         * @brief Pause reading from this connection.
         *
         * Posts a modify to the event loop that removes readable interest.
         * TCP flow control throttles the sender when the kernel buffer fills.
         */
        void pause_from_user()
        {
            if (this->closed_.load(std::memory_order_relaxed))
                return;

            paused_.store(true, std::memory_order_relaxed);

            auto self = this->shared_from_this();
            this->loop_->post(
                [self]()
                {
                    auto *s = static_cast<stream_conn_impl *>(self.get());
                    if (s->closed_.load(std::memory_order_relaxed))
                        return;

                    // Keep writable interest if we have data to flush
                    auto interest = io_event::none;
                    {
                        std::lock_guard<std::mutex> lock(s->write_mutex_);
                        if (s->write_registered_)
                            interest = io_event::writable;
                    }

                    if (interest != io_event::none)
                        s->loop_->io().modify(s->socket_.native_handle(), interest);
                    else
                        s->loop_->io().modify(s->socket_.native_handle(), io_event::none);
                });
        }

        /**
         * @brief Resume reading from this connection.
         *
         * Posts a modify to the event loop that restores readable interest.
         * Datagrams buffered in the kernel are drained immediately.
         */
        void resume_from_user()
        {
            if (this->closed_.load(std::memory_order_relaxed))
                return;

            paused_.store(false, std::memory_order_relaxed);

            auto self = this->shared_from_this();
            this->loop_->post(
                [self]()
                {
                    auto *s = static_cast<stream_conn_impl *>(self.get());
                    if (s->closed_.load(std::memory_order_relaxed))
                        return;

                    auto interest = io_event::readable;
                    {
                        std::lock_guard<std::mutex> lock(s->write_mutex_);
                        if (s->write_registered_)
                            interest |= io_event::writable;
                    }

                    s->loop_->io().modify(s->socket_.native_handle(), interest);
                });
        }
    };

} // namespace socketpp::detail

// ── impl struct definitions ──────────────────────────────────────────────────

/// @brief IPv4 stream connection implementation.
struct socketpp::stream4::connection::impl :
    socketpp::detail::stream_conn_impl<socketpp::tcp4_socket, socketpp::inet4_address>
{
    using stream_conn_impl::stream_conn_impl;
};

/// @brief IPv6 stream connection implementation.
struct socketpp::stream6::connection::impl :
    socketpp::detail::stream_conn_impl<socketpp::tcp6_socket, socketpp::inet6_address>
{
    using stream_conn_impl::stream_conn_impl;
};

#endif // SOCKETPP_DETAIL_STREAM_CONNECTION_IMPL_HPP

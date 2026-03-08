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
 * @file udp_server.cpp
 * @brief Implementation of udp4_server and udp6_server.
 *
 * The UDP server impl template manages:
 * - A bound UDP socket registered with the event loop for readable events
 * - A non-blocking recv loop that drains all available datagrams per wakeup
 * - A thread pool for dispatching on_message and on_error callbacks
 *
 * Unlike TCP, there is no connection state to track. Each datagram is
 * independent. The recv loop runs on the event loop thread and reads until
 * would_block to avoid repeated epoll/kqueue/IOCP wakeups for back-to-back
 * datagrams.
 *
 * send_to() is synchronous and performed directly on the calling thread.
 * It does not go through the event loop.
 */

#include "thread_pool.hpp"

#include <atomic>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/udp.hpp>
#include <socketpp/udp_server.hpp>
#include <thread>
#include <vector>

namespace socketpp
{

    // ── udp_server impl template ─────────────────────────────────────────────────

    namespace
    {

        template<typename ServerType, typename Socket, typename Address> struct udp_server_impl
        {
            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;
            Socket socket;

            typename ServerType::message_handler on_message_cb;
            typename ServerType::error_handler on_error_cb;

            size_t worker_count = 0;
            socket_options sock_opts = socket_options {}.reuse_addr(true);
            size_t read_buf_size = 65536; // 64 KB default; should be >= largest expected datagram

            std::vector<char> read_buf;

            std::thread background_thread;
            std::atomic<bool> started {false};

            void do_bind(const Address &addr, result<void> &out)
            {
                auto r = socket.open(addr, sock_opts);

                if (!r)
                {
                    out = r;
                    return;
                }

                socket.set_non_blocking(true);

                pool = std::make_unique<detail::thread_pool>(worker_count);
                read_buf.resize(read_buf_size);

                auto fd = socket.native_handle();

                loop.io().add(
                    fd,
                    io_event::readable,
                    [this](socket_t, io_event events)
                    {
                        if (!has_event(events, io_event::readable))
                            return;

                        // Drain available datagrams in a tight loop to minimize
                        // event loop wakeups. Capped at 256 per wakeup to prevent
                        // a sustained UDP flood from monopolizing the event loop
                        // and exhausting thread pool / memory resources.
                        constexpr size_t max_drain = 256;

                        for (size_t drain_count = 0; drain_count < max_drain; ++drain_count)
                        {
                            Address sender;
                            auto recv_r = socket.recv_from(read_buf.data(), read_buf.size(), sender);

                            if (!recv_r)
                            {
                                if (recv_r.error() == make_error_code(errc::would_block))
                                    break; // No more datagrams available right now.

                                if (on_error_cb)
                                {
                                    auto ec = recv_r.error();
                                    pool->submit([this, ec]() { on_error_cb(ec); });
                                }

                                break;
                            }

                            auto n = recv_r.value();

                            if (on_message_cb)
                            {
                                // Copy the datagram into a standalone buffer so the
                                // thread pool callback doesn't race with the next recv.
                                auto data = std::vector<char>(read_buf.data(), read_buf.data() + n);

                                pool->submit([this, d = std::move(data), sender]()
                                             { on_message_cb(d.data(), d.size(), sender); });
                            }
                        }
                    });

                out = result<void>();
            }

            /// Synchronous send -- called directly from the user's thread.
            bool do_send_to(const void *data, size_t len, const Address &dest)
            {
                auto r = socket.send_to(data, len, dest);
                return !!r;
            }

            /// Shutdown sequence. Joins background thread before closing the
            /// socket to prevent races with the recv loop.
            void do_stop()
            {
                loop.stop();

                if (background_thread.joinable())
                    background_thread.join();

                if (socket.is_open())
                {
                    loop.io().remove(socket.native_handle());
                    socket.close();
                }

                if (pool)
                    pool->shutdown();

                started.store(false, std::memory_order_relaxed);
            }
        };

    } // namespace

    // ── udp4_server ──────────────────────────────────────────────────────────────

    struct udp4_server::impl : udp_server_impl<udp4_server, udp4_socket, inet4_address>
    {
    };

    udp4_server::udp4_server(): impl_(std::make_unique<impl>()) {}
    udp4_server::~udp4_server()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    udp4_server::udp4_server(udp4_server &&) noexcept = default;
    udp4_server &udp4_server::operator=(udp4_server &&) noexcept = default;

    udp4_server &udp4_server::on_message(message_handler handler)
    {
        impl_->on_message_cb = std::move(handler);
        return *this;
    }

    udp4_server &udp4_server::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    udp4_server &udp4_server::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    udp4_server &udp4_server::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    udp4_server &udp4_server::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    result<void> udp4_server::bind(const inet4_address &addr)
    {
        result<void> out;
        impl_->do_bind(addr, out);
        return out;
    }

    void udp4_server::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void udp4_server::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void udp4_server::stop()
    {
        impl_->do_stop();
    }

    bool udp4_server::send_to(const void *data, size_t len, const inet4_address &dest)
    {
        return impl_->do_send_to(data, len, dest);
    }

    bool udp4_server::running() const noexcept
    {
        return impl_->loop.running();
    }

    // ── udp6_server ──────────────────────────────────────────────────────────────

    struct udp6_server::impl : udp_server_impl<udp6_server, udp6_socket, inet6_address>
    {
    };

    udp6_server::udp6_server(): impl_(std::make_unique<impl>()) {}
    udp6_server::~udp6_server()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    udp6_server::udp6_server(udp6_server &&) noexcept = default;
    udp6_server &udp6_server::operator=(udp6_server &&) noexcept = default;

    udp6_server &udp6_server::on_message(message_handler handler)
    {
        impl_->on_message_cb = std::move(handler);
        return *this;
    }

    udp6_server &udp6_server::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    udp6_server &udp6_server::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    udp6_server &udp6_server::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    udp6_server &udp6_server::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    result<void> udp6_server::bind(const inet6_address &addr)
    {
        result<void> out;
        impl_->do_bind(addr, out);
        return out;
    }

    void udp6_server::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void udp6_server::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void udp6_server::stop()
    {
        impl_->do_stop();
    }

    bool udp6_server::send_to(const void *data, size_t len, const inet6_address &dest)
    {
        return impl_->do_send_to(data, len, dest);
    }

    bool udp6_server::running() const noexcept
    {
        return impl_->loop.running();
    }

} // namespace socketpp

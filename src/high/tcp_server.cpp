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
 * @file tcp_server.cpp
 * @brief Implementation of tcp4_server and tcp6_server.
 *
 * The server impl template manages:
 * - A listening socket that feeds accepted connections into the event loop
 * - A connections map (socket_t -> conn_entry) that owns each connection's
 *   lifetime via shared_ptr
 * - A thread pool for dispatching user callbacks off the event loop thread
 *
 * Connection lifetime flow:
 * 1. accept_loop callback fires on the event loop thread
 * 2. A Connection wrapper (shared_ptr) is created and stored in the map
 * 3. on_connect runs on the thread pool; user sets up callbacks and may send()
 * 4. start_reading() registers the socket with the I/O dispatcher
 * 5. On close, the wrapper is erased from the map via an event loop post
 *
 * The wrapper in the map keeps the Connection alive so that user callbacks
 * capturing &conn remain valid.
 */

#include "tcp_connection_impl.hpp"

#include <cstring>
#include <socketpp/socket/tcp_listener.hpp>
#include <socketpp/tcp_server.hpp>
#include <thread>
#include <unordered_map>

namespace socketpp
{

    // ── tcp_server impl template ─────────────────────────────────────────────────

    namespace
    {

        template<typename ServerType, typename Listener, typename Socket, typename Address, typename Connection>
        struct tcp_server_impl
        {
            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;
            Listener listener;

            typename ServerType::connect_handler on_connect_cb;
            typename ServerType::error_handler on_error_cb;

            size_t worker_count = 0;
            size_t max_write_buf = 16 * 1024 * 1024;   // 16 MB default per-connection write limit
            size_t read_buf_size = 65536;               // 64 KB default per-connection read buffer
            size_t max_conn = 0;                        // 0 = unlimited connections
            socket_options sock_opts = socket_options {}.reuse_addr(true).tcp_nodelay(true);
            int backlog_val = default_backlog;

            /// Entry in the connections map. Stores both the raw impl (for close
            /// during shutdown) and the wrapper (to keep the Connection alive for
            /// user callbacks).
            struct conn_entry
            {
                std::shared_ptr<typename Connection::impl> impl;
                std::shared_ptr<Connection> wrapper; ///< Keeps Connection alive for user callbacks
            };

            /// Connections keyed by native socket handle. Only modified from the
            /// event loop thread (accept callback and close-posted erasure).
            std::unordered_map<socket_t, conn_entry> connections;

            /// Thread-safe connection counter. Incremented/decremented from the
            /// event loop thread, safe to read from any thread.
            std::atomic<size_t> connection_count {0};

            std::thread background_thread;
            std::atomic<bool> started {false};

            void do_listen(const Address &addr, result<void> &out)
            {
                auto r = listener.open(addr, backlog_val, sock_opts);

                if (!r)
                {
                    out = r;
                    return;
                }

                pool = std::make_unique<detail::thread_pool>(worker_count);

                listener.accept_loop(
                    loop,
                    [this](result<Socket> sock_r, Address peer)
                    {
                        if (!sock_r)
                        {
                            if (on_error_cb)
                            {
                                auto ec = sock_r.error();
                                pool->submit([this, ec]() { on_error_cb(ec); });
                            }
                            return;
                        }

                        auto sock = std::move(sock_r.value());

                        // Enforce max connection limit. If at capacity, close the
                        // newly accepted socket immediately to shed load.
                        if (max_conn > 0 && connections.size() >= max_conn)
                        {
                            sock.close();
                            return;
                        }

                        auto fd = sock.native_handle();

                        sock.set_non_blocking(true);

                        Address local_addr;
                        auto local_r = sock.local_addr();
                        if (local_r)
                            local_addr = local_r.value();

                        auto conn_impl = std::make_shared<typename Connection::impl>(
                            std::move(sock), peer, local_addr, loop, *pool, read_buf_size, max_write_buf);

                        if (on_connect_cb)
                        {
                            // Create a shared wrapper so the Connection stays alive for the
                            // duration of user callbacks that capture &conn.
                            auto conn_ptr = std::make_shared<Connection>(conn_impl);
                            connections[fd] = {conn_impl, conn_ptr};
                            connection_count.fetch_add(1, std::memory_order_relaxed);

                            pool->submit(
                                [this, conn_ptr, fd]()
                                {
                                    // Deliver the connection to the user. They can set up
                                    // on_data/on_close/on_error and call send() here.
                                    on_connect_cb(*conn_ptr);

                                    // Wrap the user's on_close handler (if any) to also
                                    // remove the connection from the map. The map erasure
                                    // is posted to the event loop to avoid concurrent
                                    // modification.
                                    auto user_close = conn_ptr->impl_->on_close_;
                                    conn_ptr->impl_->on_close_ = [this, fd, user_close]()
                                    {
                                        loop.post(
                                            [this, fd]()
                                            {
                                                connections.erase(fd);
                                                connection_count.fetch_sub(1, std::memory_order_relaxed);
                                            });

                                        if (user_close)
                                            user_close();
                                    };

                                    // Now register with the dispatcher. start_reading()
                                    // checks if send() was called during on_connect and
                                    // includes writable interest if so.
                                    conn_ptr->impl_->start_reading();
                                });
                        }
                        else
                        {
                            // No user callback -- still track the connection for cleanup.
                            connections[fd] = {conn_impl, nullptr};
                            connection_count.fetch_add(1, std::memory_order_relaxed);

                            conn_impl->on_close_ = [this, fd]()
                            {
                                loop.post(
                                    [this, fd]()
                                    {
                                        connections.erase(fd);
                                        connection_count.fetch_sub(1, std::memory_order_relaxed);
                                    });
                            };

                            conn_impl->start_reading();
                        }
                    });

                out = result<void>();
            }

            /// Shutdown sequence. Order is critical:
            /// 1. Stop the event loop (unblocks run())
            /// 2. Stop the listener (stops accepting)
            /// 3. Join the background thread -- this MUST happen before touching
            ///    the connections map to avoid data races
            /// 4. Close all remaining connections
            /// 5. Shut down the thread pool (drains pending callbacks)
            void do_stop()
            {
                loop.stop();
                listener.stop();

                // Join the event loop thread first so no more events are processed.
                // This ensures the connections map is not being modified concurrently
                // when we iterate it below.
                if (background_thread.joinable())
                    background_thread.join();

                for (auto &[fd, entry] : connections)
                    entry.impl->initiate_close();

                connections.clear();
                connection_count.store(0, std::memory_order_relaxed);

                if (pool)
                    pool->shutdown();

                started.store(false, std::memory_order_relaxed);
            }
        };

    } // namespace

    // ── tcp4_server ──────────────────────────────────────────────────────────────

    struct tcp4_server::impl : tcp_server_impl<tcp4_server, tcp4_listener, tcp4_socket, inet4_address, tcp4_connection>
    {
    };

    tcp4_server::tcp4_server(): impl_(std::make_unique<impl>()) {}
    tcp4_server::~tcp4_server()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    tcp4_server::tcp4_server(tcp4_server &&) noexcept = default;
    tcp4_server &tcp4_server::operator=(tcp4_server &&) noexcept = default;

    tcp4_server &tcp4_server::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);
        return *this;
    }

    tcp4_server &tcp4_server::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    tcp4_server &tcp4_server::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    tcp4_server &tcp4_server::max_write_buffer(size_t bytes)
    {
        impl_->max_write_buf = bytes;
        return *this;
    }

    tcp4_server &tcp4_server::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    tcp4_server &tcp4_server::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    tcp4_server &tcp4_server::backlog(int value)
    {
        impl_->backlog_val = value;
        return *this;
    }

    tcp4_server &tcp4_server::max_connections(size_t count)
    {
        impl_->max_conn = count;
        return *this;
    }

    result<void> tcp4_server::listen(const inet4_address &addr)
    {
        result<void> out;
        impl_->do_listen(addr, out);
        return out;
    }

    void tcp4_server::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void tcp4_server::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void tcp4_server::stop()
    {
        impl_->do_stop();
    }

    bool tcp4_server::running() const noexcept
    {
        return impl_->loop.running();
    }

    size_t tcp4_server::connection_count() const noexcept
    {
        return impl_->connection_count.load(std::memory_order_relaxed);
    }

    inet4_address tcp4_server::listening_address() const
    {
        // Retrieve the bound address from the underlying listener socket.
        // Uses memcpy because local_address() returns a generic sockaddr buffer.
        auto r = impl_->listener.handle().local_address();
        if (!r)
            return {};

        inet4_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

    // ── tcp6_server ──────────────────────────────────────────────────────────────

    struct tcp6_server::impl : tcp_server_impl<tcp6_server, tcp6_listener, tcp6_socket, inet6_address, tcp6_connection>
    {
    };

    tcp6_server::tcp6_server(): impl_(std::make_unique<impl>()) {}
    tcp6_server::~tcp6_server()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    tcp6_server::tcp6_server(tcp6_server &&) noexcept = default;
    tcp6_server &tcp6_server::operator=(tcp6_server &&) noexcept = default;

    tcp6_server &tcp6_server::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);
        return *this;
    }

    tcp6_server &tcp6_server::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    tcp6_server &tcp6_server::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    tcp6_server &tcp6_server::max_write_buffer(size_t bytes)
    {
        impl_->max_write_buf = bytes;
        return *this;
    }

    tcp6_server &tcp6_server::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    tcp6_server &tcp6_server::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    tcp6_server &tcp6_server::backlog(int value)
    {
        impl_->backlog_val = value;
        return *this;
    }

    tcp6_server &tcp6_server::max_connections(size_t count)
    {
        impl_->max_conn = count;
        return *this;
    }

    result<void> tcp6_server::listen(const inet6_address &addr)
    {
        result<void> out;
        impl_->do_listen(addr, out);
        return out;
    }

    void tcp6_server::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void tcp6_server::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void tcp6_server::stop()
    {
        impl_->do_stop();
    }

    bool tcp6_server::running() const noexcept
    {
        return impl_->loop.running();
    }

    size_t tcp6_server::connection_count() const noexcept
    {
        return impl_->connection_count.load(std::memory_order_relaxed);
    }

    inet6_address tcp6_server::listening_address() const
    {
        auto r = impl_->listener.handle().local_address();
        if (!r)
            return {};

        inet6_address addr;
        std::memcpy(&addr, r.value().data(), sizeof(addr));
        return addr;
    }

} // namespace socketpp

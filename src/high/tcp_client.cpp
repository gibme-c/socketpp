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
 * @file tcp_client.cpp
 * @brief Implementation of tcp4_client and tcp6_client.
 *
 * The client impl template manages:
 * - An asynchronous outbound connection via the Connector abstraction
 * - A single connection's lifetime via shared_ptr (connection + wrapper)
 * - A thread pool for dispatching user callbacks off the event loop thread
 *
 * Connection lifetime flow:
 * 1. connect_async callback fires on the event loop thread when the
 *    connection completes (or times out / fails)
 * 2. A Connection wrapper (shared_ptr) is created and stored as a member
 * 3. on_connect runs on the thread pool; user sets up callbacks and may send()
 * 4. start_reading() registers the socket with the I/O dispatcher
 * 5. On close, the connected_flag is cleared and the user's on_close fires
 *
 * Stop ordering follows the same pattern as tcp_server: join the event loop
 * thread BEFORE touching connection state to avoid data races.
 */

#include "tcp_connection_impl.hpp"

#include <socketpp/socket/tcp_connector.hpp>
#include <socketpp/tcp_client.hpp>
#include <thread>

namespace socketpp
{

    // ── tcp_client impl template ─────────────────────────────────────────────────

    namespace
    {

        template<typename ClientType, typename Connector, typename Socket, typename Address, typename Connection>
        struct tcp_client_impl
        {
            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;

            typename ClientType::connect_handler on_connect_cb;
            typename ClientType::error_handler on_error_cb;

            size_t worker_count = 0;
            size_t max_write_buf = 16 * 1024 * 1024;   // 16 MB default write limit
            size_t read_buf_size = 65536;               // 64 KB default read buffer
            socket_options sock_opts = socket_options {}.tcp_nodelay(true);
            std::chrono::milliseconds timeout {30000};  // 30 second default connect timeout

            std::shared_ptr<typename Connection::impl> connection;
            std::shared_ptr<Connection> connection_wrapper; ///< Keeps Connection alive for user callbacks

            std::thread background_thread;
            std::atomic<bool> started {false};
            std::atomic<bool> connected_flag {false};

            void do_connect(const Address &addr)
            {
                pool = std::make_unique<detail::thread_pool>(worker_count);

                Connector::connect_async(
                    loop,
                    addr,
                    [this](result<Socket> sock_r)
                    {
                        if (!sock_r)
                        {
                            // Connection failed or timed out.
                            if (on_error_cb)
                            {
                                auto ec = sock_r.error();
                                pool->submit([this, ec]() { on_error_cb(ec); });
                            }
                            return;
                        }

                        auto sock = std::move(sock_r.value());
                        sock.set_non_blocking(true);

                        Address peer_addr, local_addr;
                        auto peer_r = sock.peer_addr();
                        if (peer_r)
                            peer_addr = peer_r.value();
                        auto local_r = sock.local_addr();
                        if (local_r)
                            local_addr = local_r.value();

                        connection = std::make_shared<typename Connection::impl>(
                            std::move(sock), peer_addr, local_addr, loop, *pool, read_buf_size, max_write_buf);

                        connected_flag.store(true, std::memory_order_relaxed);

                        if (on_connect_cb)
                        {
                            // Create a wrapper so the Connection stays alive for user callbacks.
                            connection_wrapper = std::make_shared<Connection>(connection);
                            auto conn_ptr = connection_wrapper;

                            pool->submit(
                                [this, conn_ptr]()
                                {
                                    // Deliver the connection to the user. They can set up
                                    // on_data/on_close/on_error and call send() here.
                                    on_connect_cb(*conn_ptr);

                                    // Wrap the user's on_close to also clear the connected flag.
                                    auto user_close = conn_ptr->impl_->on_close_;
                                    conn_ptr->impl_->on_close_ = [this, user_close]()
                                    {
                                        connected_flag.store(false, std::memory_order_relaxed);

                                        if (user_close)
                                            user_close();
                                    };

                                    // Register with the dispatcher. start_reading() checks
                                    // if send() was called during on_connect.
                                    conn_ptr->impl_->start_reading();
                                });
                        }
                        else
                        {
                            // No user callback -- just start reading.
                            connection->start_reading();
                        }
                    },
                    timeout,
                    sock_opts);
            }

            /// Shutdown sequence -- same ordering rationale as tcp_server_impl::do_stop().
            void do_stop()
            {
                loop.stop();

                // Join the event loop thread BEFORE touching connection state.
                if (background_thread.joinable())
                    background_thread.join();

                if (connection)
                {
                    connection->initiate_close();
                    connection.reset();
                    connection_wrapper.reset();
                }

                if (pool)
                    pool->shutdown();

                started.store(false, std::memory_order_relaxed);
                connected_flag.store(false, std::memory_order_relaxed);
            }
        };

    } // namespace

    // ── tcp4_client ──────────────────────────────────────────────────────────────

    struct tcp4_client::impl : tcp_client_impl<tcp4_client, tcp4_connector, tcp4_socket, inet4_address, tcp4_connection>
    {
    };

    tcp4_client::tcp4_client(): impl_(std::make_unique<impl>()) {}
    tcp4_client::~tcp4_client()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    tcp4_client::tcp4_client(tcp4_client &&) noexcept = default;
    tcp4_client &tcp4_client::operator=(tcp4_client &&) noexcept = default;

    tcp4_client &tcp4_client::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);
        return *this;
    }

    tcp4_client &tcp4_client::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    tcp4_client &tcp4_client::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    tcp4_client &tcp4_client::max_write_buffer(size_t bytes)
    {
        impl_->max_write_buf = bytes;
        return *this;
    }

    tcp4_client &tcp4_client::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    tcp4_client &tcp4_client::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    tcp4_client &tcp4_client::connect_timeout(std::chrono::milliseconds ms)
    {
        impl_->timeout = ms;
        return *this;
    }

    void tcp4_client::connect(const inet4_address &addr)
    {
        impl_->do_connect(addr);
    }

    void tcp4_client::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void tcp4_client::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void tcp4_client::stop()
    {
        impl_->do_stop();
    }

    bool tcp4_client::connected() const noexcept
    {
        return impl_->connected_flag.load(std::memory_order_relaxed);
    }

    // ── tcp6_client ──────────────────────────────────────────────────────────────

    struct tcp6_client::impl : tcp_client_impl<tcp6_client, tcp6_connector, tcp6_socket, inet6_address, tcp6_connection>
    {
    };

    tcp6_client::tcp6_client(): impl_(std::make_unique<impl>()) {}
    tcp6_client::~tcp6_client()
    {
        if (impl_ && impl_->started.load(std::memory_order_relaxed))
            impl_->do_stop();
    }

    tcp6_client::tcp6_client(tcp6_client &&) noexcept = default;
    tcp6_client &tcp6_client::operator=(tcp6_client &&) noexcept = default;

    tcp6_client &tcp6_client::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);
        return *this;
    }

    tcp6_client &tcp6_client::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    tcp6_client &tcp6_client::worker_threads(size_t count)
    {
        impl_->worker_count = count;
        return *this;
    }

    tcp6_client &tcp6_client::max_write_buffer(size_t bytes)
    {
        impl_->max_write_buf = bytes;
        return *this;
    }

    tcp6_client &tcp6_client::read_buffer_size(size_t bytes)
    {
        impl_->read_buf_size = bytes;
        return *this;
    }

    tcp6_client &tcp6_client::socket_opts(const socket_options &opts)
    {
        impl_->sock_opts = opts;
        return *this;
    }

    tcp6_client &tcp6_client::connect_timeout(std::chrono::milliseconds ms)
    {
        impl_->timeout = ms;
        return *this;
    }

    void tcp6_client::connect(const inet6_address &addr)
    {
        impl_->do_connect(addr);
    }

    void tcp6_client::run()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->loop.run();
    }

    void tcp6_client::start()
    {
        impl_->started.store(true, std::memory_order_relaxed);
        impl_->background_thread = std::thread([this]() { impl_->loop.run(); });
    }

    void tcp6_client::stop()
    {
        impl_->do_stop();
    }

    bool tcp6_client::connected() const noexcept
    {
        return impl_->connected_flag.load(std::memory_order_relaxed);
    }

} // namespace socketpp

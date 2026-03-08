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
 * @file stream.cpp
 * @brief Implementation of stream4, stream6, and their nested connection types.
 *
 * Single impl struct per stream type with a mode enum. Factory methods populate
 * the relevant subset of fields. Callback arming via on_connect() eliminates
 * races between factory return and callback setup.
 */

#include "serial_queue.hpp"
#include "stream_connection_impl.hpp"

#include <cstring>
#include <future>
#include <socketpp/socket/tcp_connector.hpp>
#include <socketpp/socket/tcp_listener.hpp>
#include <thread>
#include <unordered_map>

namespace socketpp
{

    // ── stream impl template ─────────────────────────────────────────────────────

    namespace
    {

        enum class stream_mode
        {
            listen,
            client
        };

        template<typename StreamType, typename Listener, typename Connector, typename Socket, typename Address>
        struct stream_impl
        {
            using Connection = typename StreamType::connection;
            using ConnImpl = typename Connection::impl;

            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;
            std::unique_ptr<detail::serial_queue> serial; ///< Serializes container-level timer/post callbacks.

            stream_mode mode_;

            // Callbacks
            typename StreamType::connect_handler on_connect_cb;
            typename StreamType::error_handler on_error_cb;

            // ── Listen-mode fields ──────────────────────────────────────────
            Listener listener;
            size_t max_write_buf = 16 * 1024 * 1024;
            size_t read_buf_size = 65536;
            size_t max_conn = 0;
            socket_options sock_opts;
            int backlog_val = default_backlog;

            struct conn_entry
            {
                std::shared_ptr<ConnImpl> impl;
                std::shared_ptr<Connection> wrapper;
            };

            std::unordered_map<socket_t, conn_entry> connections;
            std::atomic<size_t> connection_count {0};
            std::atomic<bool> listen_paused {false};

            // ── Connect-mode fields ─────────────────────────────────────────
            Address connect_addr;
            std::chrono::milliseconds connect_timeout {30000};
            std::shared_ptr<ConnImpl> client_conn;
            std::shared_ptr<Connection> client_conn_wrapper;
            std::atomic<bool> connected_flag {false};

            std::thread background_thread;
            std::atomic<bool> armed {false};

            // ── Factory: listen mode ────────────────────────────────────────

            static result<void> do_listen(stream_impl &self, const Address &addr, const stream_listen_config &config)
            {
                self.mode_ = stream_mode::listen;
                self.max_write_buf = config.max_write_buffer;
                self.read_buf_size = config.read_buffer_size;
                self.max_conn = config.max_connections;
                self.sock_opts = config.sock_opts;
                self.backlog_val = config.backlog;

                auto r = self.listener.open(addr, self.backlog_val, self.sock_opts);

                if (!r)
                    return r;

                self.pool = std::make_unique<detail::thread_pool>(config.worker_threads);
                self.serial = std::make_unique<detail::serial_queue>(self.pool.get());

                // Start event loop but don't arm accept loop yet
                self.background_thread = std::thread([&self]() { self.loop.run(); });

                return {};
            }

            // ── Factory: connect mode ───────────────────────────────────────

            static result<void> do_connect(stream_impl &self, const Address &addr, const stream_connect_config &config)
            {
                self.mode_ = stream_mode::client;
                self.connect_addr = addr;
                self.connect_timeout = config.connect_timeout;
                self.max_write_buf = config.max_write_buffer;
                self.read_buf_size = config.read_buffer_size;
                self.sock_opts = config.sock_opts;

                self.pool = std::make_unique<detail::thread_pool>(config.worker_threads);
                self.serial = std::make_unique<detail::serial_queue>(self.pool.get());

                // Start event loop but don't initiate connection yet
                self.background_thread = std::thread([&self]() { self.loop.run(); });

                return {};
            }

            // ── Arm: called by on_connect() ─────────────────────────────────

            void arm_listen()
            {
                if (armed.exchange(true, std::memory_order_acq_rel))
                    return;

                loop.post(
                    [this]()
                    {
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

                                auto ci = std::make_shared<ConnImpl>(
                                    std::move(sock), peer, local_addr, loop, *pool, read_buf_size, max_write_buf);

                                if (on_connect_cb)
                                {
                                    auto conn_ptr = std::make_shared<Connection>(ci);
                                    connections[fd] = {ci, conn_ptr};
                                    connection_count.fetch_add(1, std::memory_order_relaxed);

                                    pool->submit(
                                        [this, conn_ptr, fd]()
                                        {
                                            on_connect_cb(*conn_ptr);

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

                                            conn_ptr->impl_->start_reading();
                                        });
                                }
                                else
                                {
                                    connections[fd] = {ci, nullptr};
                                    connection_count.fetch_add(1, std::memory_order_relaxed);

                                    ci->on_close_ = [this, fd]()
                                    {
                                        loop.post(
                                            [this, fd]()
                                            {
                                                connections.erase(fd);
                                                connection_count.fetch_sub(1, std::memory_order_relaxed);
                                            });
                                    };

                                    ci->start_reading();
                                }
                            });
                    });
            }

            void arm_connect()
            {
                if (armed.exchange(true, std::memory_order_acq_rel))
                    return;

                loop.post(
                    [this]()
                    {
                        Connector::connect_async(
                            loop,
                            connect_addr,
                            [this](result<Socket> sock_r)
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
                                sock.set_non_blocking(true);

                                Address peer_addr, local_addr;
                                auto peer_r = sock.peer_addr();
                                if (peer_r)
                                    peer_addr = peer_r.value();
                                auto local_r = sock.local_addr();
                                if (local_r)
                                    local_addr = local_r.value();

                                client_conn = std::make_shared<ConnImpl>(
                                    std::move(sock), peer_addr, local_addr, loop, *pool, read_buf_size, max_write_buf);

                                connected_flag.store(true, std::memory_order_relaxed);

                                if (on_connect_cb)
                                {
                                    client_conn_wrapper = std::make_shared<Connection>(client_conn);
                                    auto conn_ptr = client_conn_wrapper;

                                    pool->submit(
                                        [this, conn_ptr]()
                                        {
                                            on_connect_cb(*conn_ptr);

                                            auto user_close = conn_ptr->impl_->on_close_;
                                            conn_ptr->impl_->on_close_ = [this, user_close]()
                                            {
                                                connected_flag.store(false, std::memory_order_relaxed);

                                                if (user_close)
                                                    user_close();
                                            };

                                            conn_ptr->impl_->start_reading();
                                        });
                                }
                                else
                                {
                                    client_conn->start_reading();
                                }
                            },
                            connect_timeout,
                            sock_opts);
                    });
            }

            // ── pause/resume (listen mode) ──────────────────────────────────

            void do_pause()
            {
                if (mode_ != stream_mode::listen || !armed.load(std::memory_order_relaxed))
                    return;

                listen_paused.store(true, std::memory_order_relaxed);
                loop.post([this]() { listener.stop(); });
            }

            void do_resume()
            {
                if (mode_ != stream_mode::listen || !armed.load(std::memory_order_relaxed))
                    return;

                listen_paused.store(false, std::memory_order_relaxed);

                // Re-arm the accept loop
                loop.post(
                    [this]()
                    {
                        // The accept_loop needs to be re-registered. Since the listener
                        // was stopped, we need to re-add it to the event loop.
                        // accept_loop registers the listener fd for readable events.
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

                                auto ci = std::make_shared<ConnImpl>(
                                    std::move(sock), peer, local_addr, loop, *pool, read_buf_size, max_write_buf);

                                if (on_connect_cb)
                                {
                                    auto conn_ptr = std::make_shared<Connection>(ci);
                                    connections[fd] = {ci, conn_ptr};
                                    connection_count.fetch_add(1, std::memory_order_relaxed);

                                    pool->submit(
                                        [this, conn_ptr, fd]()
                                        {
                                            on_connect_cb(*conn_ptr);

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

                                            conn_ptr->impl_->start_reading();
                                        });
                                }
                                else
                                {
                                    connections[fd] = {ci, nullptr};
                                    connection_count.fetch_add(1, std::memory_order_relaxed);

                                    ci->on_close_ = [this, fd]()
                                    {
                                        loop.post(
                                            [this, fd]()
                                            {
                                                connections.erase(fd);
                                                connection_count.fetch_sub(1, std::memory_order_relaxed);
                                            });
                                    };

                                    ci->start_reading();
                                }
                            });
                    });
            }

            // ── Local address ───────────────────────────────────────────────

            Address do_local_addr() const
            {
                if (mode_ == stream_mode::listen)
                {
                    auto r = listener.handle().local_address();
                    if (!r)
                        return {};

                    Address addr;
                    std::memcpy(&addr, r.value().data(), sizeof(addr));
                    return addr;
                }
                else
                {
                    if (client_conn)
                        return client_conn->local_;
                    return {};
                }
            }

            // ── Timer / Post ────────────────────────────────────────────────

            timer_handle do_defer(std::chrono::milliseconds delay, std::function<void()> cb)
            {
                auto p = std::make_shared<std::promise<timer_handle>>();
                auto f = p->get_future();
                auto *sq = serial.get();

                loop.post(
                    [this, delay, cb = std::move(cb), p, sq]() mutable
                    {
                        auto h = loop.defer(delay, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                        p->set_value(h);
                    });

                return f.get();
            }

            timer_handle do_repeat(std::chrono::milliseconds interval, std::function<void()> cb)
            {
                auto p = std::make_shared<std::promise<timer_handle>>();
                auto f = p->get_future();
                auto *sq = serial.get();

                loop.post(
                    [this, interval, cb = std::move(cb), p, sq]() mutable
                    {
                        auto h = loop.repeat(interval, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                        p->set_value(h);
                    });

                return f.get();
            }

            void do_post(std::function<void()> cb)
            {
                auto *sq = serial.get();
                loop.post([sq, cb = std::move(cb)]() { sq->submit(cb); });
            }

            // ── Destroy ─────────────────────────────────────────────────────

            void do_destroy()
            {
                if (mode_ == stream_mode::listen)
                    listener.stop();

                loop.post([this]() { loop.stop(); });
                loop.stop();

                if (background_thread.joinable())
                    background_thread.join();

                if (mode_ == stream_mode::listen)
                {
                    for (auto &[fd, entry] : connections)
                        entry.impl->initiate_close();

                    connections.clear();
                    connection_count.store(0, std::memory_order_relaxed);
                }
                else
                {
                    if (client_conn)
                    {
                        client_conn->initiate_close();
                        client_conn.reset();
                        client_conn_wrapper.reset();
                    }

                    connected_flag.store(false, std::memory_order_relaxed);
                }

                if (pool)
                    pool->shutdown();
            }
        };

    } // namespace

    // ── stream4::connection ──────────────────────────────────────────────────────

    stream4::connection::connection(std::shared_ptr<impl> p): impl_(std::move(p)) {}

    stream4::connection::~connection()
    {
        if (impl_ && impl_.use_count() == 1 && impl_->loop_ && !impl_->closed_.load(std::memory_order_relaxed))
            impl_->close_from_user();
    }

    stream4::connection::connection(connection &&) noexcept = default;
    stream4::connection &stream4::connection::operator=(connection &&) noexcept = default;

    void stream4::connection::on_data(data_handler handler)
    {
        impl_->on_data_ = std::move(handler);
    }
    void stream4::connection::on_close(close_handler handler)
    {
        impl_->on_close_ = std::move(handler);
    }
    void stream4::connection::on_error(error_handler handler)
    {
        impl_->on_error_ = std::move(handler);
    }
    bool stream4::connection::send(const void *data, size_t len)
    {
        return impl_->enqueue_send(data, len);
    }
    bool stream4::connection::send(const std::string &data)
    {
        return impl_->enqueue_send(data.data(), data.size());
    }
    void stream4::connection::close()
    {
        impl_->close_from_user();
    }
    void stream4::connection::pause()
    {
        impl_->pause_from_user();
    }
    void stream4::connection::resume()
    {
        impl_->resume_from_user();
    }
    bool stream4::connection::paused() const noexcept
    {
        return impl_->paused_.load(std::memory_order_relaxed);
    }
    inet4_address stream4::connection::peer_addr() const
    {
        return impl_->peer_;
    }
    inet4_address stream4::connection::local_addr() const
    {
        return impl_->local_;
    }
    bool stream4::connection::is_open() const noexcept
    {
        return !impl_->closed_.load(std::memory_order_relaxed);
    }

    size_t stream4::connection::write_queue_bytes() const noexcept
    {
        std::lock_guard<std::mutex> lock(impl_->write_mutex_);
        return impl_->write_queue_bytes_;
    }

    // ── stream4 ──────────────────────────────────────────────────────────────────

    struct stream4::impl : stream_impl<stream4, tcp4_listener, tcp4_connector, tcp4_socket, inet4_address>
    {
    };

    stream4::stream4(): impl_(std::make_unique<impl>()) {}

    stream4::~stream4()
    {
        if (impl_)
            impl_->do_destroy();
    }

    stream4::stream4(stream4 &&) noexcept = default;
    stream4 &stream4::operator=(stream4 &&) noexcept = default;

    result<stream4> stream4::listen(inet4_address addr, stream_listen_config config)
    {
        stream4 s;
        auto r = impl::do_listen(*s.impl_, addr, config);

        if (!r)
            return r.error();

        return s;
    }

    result<stream4> stream4::connect(inet4_address addr, stream_connect_config config)
    {
        stream4 s;
        auto r = impl::do_connect(*s.impl_, addr, config);

        if (!r)
            return r.error();

        return s;
    }

    stream4 &stream4::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);

        if (impl_->mode_ == stream_mode::listen)
            impl_->arm_listen();
        else
            impl_->arm_connect();

        return *this;
    }

    stream4 &stream4::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    timer_handle stream4::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle stream4::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void stream4::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

    void stream4::pause()
    {
        impl_->do_pause();
    }
    void stream4::resume()
    {
        impl_->do_resume();
    }

    size_t stream4::connection_count() const noexcept
    {
        return impl_->connection_count.load(std::memory_order_relaxed);
    }

    inet4_address stream4::local_addr() const
    {
        return impl_->do_local_addr();
    }

    // ── stream6::connection ──────────────────────────────────────────────────────

    stream6::connection::connection(std::shared_ptr<impl> p): impl_(std::move(p)) {}

    stream6::connection::~connection()
    {
        if (impl_ && impl_.use_count() == 1 && impl_->loop_ && !impl_->closed_.load(std::memory_order_relaxed))
            impl_->close_from_user();
    }

    stream6::connection::connection(connection &&) noexcept = default;
    stream6::connection &stream6::connection::operator=(connection &&) noexcept = default;

    void stream6::connection::on_data(data_handler handler)
    {
        impl_->on_data_ = std::move(handler);
    }
    void stream6::connection::on_close(close_handler handler)
    {
        impl_->on_close_ = std::move(handler);
    }
    void stream6::connection::on_error(error_handler handler)
    {
        impl_->on_error_ = std::move(handler);
    }
    bool stream6::connection::send(const void *data, size_t len)
    {
        return impl_->enqueue_send(data, len);
    }
    bool stream6::connection::send(const std::string &data)
    {
        return impl_->enqueue_send(data.data(), data.size());
    }
    void stream6::connection::close()
    {
        impl_->close_from_user();
    }
    void stream6::connection::pause()
    {
        impl_->pause_from_user();
    }
    void stream6::connection::resume()
    {
        impl_->resume_from_user();
    }
    bool stream6::connection::paused() const noexcept
    {
        return impl_->paused_.load(std::memory_order_relaxed);
    }
    inet6_address stream6::connection::peer_addr() const
    {
        return impl_->peer_;
    }
    inet6_address stream6::connection::local_addr() const
    {
        return impl_->local_;
    }
    bool stream6::connection::is_open() const noexcept
    {
        return !impl_->closed_.load(std::memory_order_relaxed);
    }

    size_t stream6::connection::write_queue_bytes() const noexcept
    {
        std::lock_guard<std::mutex> lock(impl_->write_mutex_);
        return impl_->write_queue_bytes_;
    }

    // ── stream6 ──────────────────────────────────────────────────────────────────

    struct stream6::impl : stream_impl<stream6, tcp6_listener, tcp6_connector, tcp6_socket, inet6_address>
    {
    };

    stream6::stream6(): impl_(std::make_unique<impl>()) {}

    stream6::~stream6()
    {
        if (impl_)
            impl_->do_destroy();
    }

    stream6::stream6(stream6 &&) noexcept = default;
    stream6 &stream6::operator=(stream6 &&) noexcept = default;

    result<stream6> stream6::listen(inet6_address addr, stream_listen_config config)
    {
        stream6 s;
        auto r = impl::do_listen(*s.impl_, addr, config);

        if (!r)
            return r.error();

        return s;
    }

    result<stream6> stream6::connect(inet6_address addr, stream_connect_config config)
    {
        stream6 s;
        auto r = impl::do_connect(*s.impl_, addr, config);

        if (!r)
            return r.error();

        return s;
    }

    stream6 &stream6::on_connect(connect_handler handler)
    {
        impl_->on_connect_cb = std::move(handler);

        if (impl_->mode_ == stream_mode::listen)
            impl_->arm_listen();
        else
            impl_->arm_connect();

        return *this;
    }

    stream6 &stream6::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    timer_handle stream6::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle stream6::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void stream6::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

    void stream6::pause()
    {
        impl_->do_pause();
    }
    void stream6::resume()
    {
        impl_->do_resume();
    }

    size_t stream6::connection_count() const noexcept
    {
        return impl_->connection_count.load(std::memory_order_relaxed);
    }

    inet6_address stream6::local_addr() const
    {
        return impl_->do_local_addr();
    }

} // namespace socketpp

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
 * @file dgram.cpp
 * @brief Implementation of dgram4 and dgram6.
 *
 * The dgram impl template manages:
 * - A bound UDP socket with deferred readable registration (armed by on_data)
 * - A non-blocking recv loop that drains all available datagrams per wakeup
 * - A thread pool for dispatching on_data and on_error callbacks
 * - pause()/resume() flow control via event loop posts
 *
 * Unlike the old udp_server, the factory creates the object fully initialized
 * (socket open, event loop running) and the destructor tears it all down.
 * There is no start()/stop()/run().
 */

#include "../platform/detect_internal.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cstring>
#include <socketpp/dgram.hpp>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/udp.hpp>
#include <thread>
#include <vector>

namespace socketpp
{

    // ── dgram impl template ──────────────────────────────────────────────────────

    namespace
    {

        template<typename DgramType, typename Socket, typename Address> struct dgram_impl
        {
            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;
            Socket socket;

            typename DgramType::data_handler on_data_cb;
            typename DgramType::error_handler on_error_cb;

            std::vector<char> read_buf;

            std::thread background_thread;
            std::atomic<bool> armed {false};
            std::atomic<bool> paused {false};

            /// Factory initialization: open socket, apply SO flags, bind, start loop.
            static result<void> do_create(dgram_impl &self, const Address &addr, const dgram_config &config)
            {
                auto effective_opts = config.sock_opts;
#if defined(SOCKETPP_OS_WINDOWS)
                if (!effective_opts.has_reuse_addr())
                    effective_opts.exclusive_addr(true);
#endif

                auto r = self.socket.open(addr, effective_opts);

                if (!r)
                    return r;

                self.socket.set_non_blocking(true);

                self.pool = std::make_unique<detail::thread_pool>(config.worker_threads);
                self.read_buf.resize(config.read_buffer_size);

                // Start the event loop on a background thread. The socket is NOT
                // registered for reading yet -- that happens in arm().
                self.background_thread = std::thread([&self]() { self.loop.run(); });

                return {};
            }

            /// Called by on_data() to register the socket for readable events.
            void arm()
            {
                if (armed.exchange(true, std::memory_order_acq_rel))
                    return; // Already armed.

                auto fd = socket.native_handle();

                loop.post(
                    [this, fd]()
                    {
                        loop.io().add(
                            fd,
                            io_event::readable,
                            [this](socket_t, io_event events)
                            {
                                if (!has_event(events, io_event::readable))
                                    return;

                                constexpr size_t max_drain = 256;

                                for (size_t drain_count = 0; drain_count < max_drain; ++drain_count)
                                {
                                    Address sender;
                                    auto recv_r = socket.recv_from(read_buf.data(), read_buf.size(), sender);

                                    if (!recv_r)
                                    {
                                        if (recv_r.error() == make_error_code(errc::would_block))
                                            break;

                                        if (on_error_cb)
                                        {
                                            auto ec = recv_r.error();
                                            pool->submit([this, ec]() { on_error_cb(ec); });
                                        }

                                        break;
                                    }

                                    auto n = recv_r.value();

                                    if (on_data_cb)
                                    {
                                        auto data = std::vector<char>(read_buf.data(), read_buf.data() + n);

                                        pool->submit([this, d = std::move(data), sender]()
                                                     { on_data_cb(d.data(), d.size(), sender); });
                                    }
                                }
                            });
                    });
            }

            /// Remove readable interest from the event loop.
            void do_pause()
            {
                if (!armed.load(std::memory_order_relaxed))
                    return;

                paused.store(true, std::memory_order_relaxed);

                auto fd = socket.native_handle();
                loop.post([this, fd]() { loop.io().remove(fd); });
            }

            /// Re-register readable interest with the event loop.
            void do_resume()
            {
                if (!armed.load(std::memory_order_relaxed))
                    return;

                paused.store(false, std::memory_order_relaxed);

                auto fd = socket.native_handle();

                loop.post(
                    [this, fd]()
                    {
                        loop.io().add(
                            fd,
                            io_event::readable,
                            [this](socket_t, io_event events)
                            {
                                if (!has_event(events, io_event::readable))
                                    return;

                                constexpr size_t max_drain = 256;

                                for (size_t drain_count = 0; drain_count < max_drain; ++drain_count)
                                {
                                    Address sender;
                                    auto recv_r = socket.recv_from(read_buf.data(), read_buf.size(), sender);

                                    if (!recv_r)
                                    {
                                        if (recv_r.error() == make_error_code(errc::would_block))
                                            break;

                                        if (on_error_cb)
                                        {
                                            auto ec = recv_r.error();
                                            pool->submit([this, ec]() { on_error_cb(ec); });
                                        }

                                        break;
                                    }

                                    auto n = recv_r.value();

                                    if (on_data_cb)
                                    {
                                        auto data = std::vector<char>(read_buf.data(), read_buf.data() + n);

                                        pool->submit([this, d = std::move(data), sender]()
                                                     { on_data_cb(d.data(), d.size(), sender); });
                                    }
                                }
                            });
                    });
            }

            /// Synchronous send -- called directly from the user's thread.
            bool do_send_to(const void *data, size_t len, const Address &dest)
            {
                auto r = socket.send_to(data, len, dest);
                return !!r;
            }

            /// Get the local bound address.
            Address do_local_addr() const
            {
                auto r = socket.local_addr();
                if (!r)
                    return {};
                return r.value();
            }

            /// Shutdown sequence: stop loop, join thread, close socket, shutdown pool.
            ///
            /// Uses post() to send the stop signal INSIDE the event loop. This
            /// avoids a race where loop.stop() is called before run() starts
            /// (run() resets stop_requested_ to false, losing a direct stop()).
            /// The posted callback executes after run()'s reset, guaranteeing
            /// the stop takes effect. The direct stop() call handles the edge
            /// case where the loop hasn't started polling yet.
            void do_destroy()
            {
                loop.post([this]() { loop.stop(); });
                loop.stop();

                if (background_thread.joinable())
                    background_thread.join();

                if (socket.is_open())
                {
                    if (armed.load(std::memory_order_relaxed) && !paused.load(std::memory_order_relaxed))
                        loop.io().remove(socket.native_handle());

                    socket.close();
                }

                if (pool)
                    pool->shutdown();
            }
        };

    } // namespace

    // ── dgram4 ───────────────────────────────────────────────────────────────────

    struct dgram4::impl : dgram_impl<dgram4, udp4_socket, inet4_address>
    {
    };

    dgram4::dgram4(): impl_(std::make_unique<impl>()) {}

    dgram4::~dgram4()
    {
        if (impl_)
            impl_->do_destroy();
    }

    dgram4::dgram4(dgram4 &&) noexcept = default;
    dgram4 &dgram4::operator=(dgram4 &&) noexcept = default;

    result<dgram4> dgram4::create(inet4_address bind_addr, dgram_config config)
    {
        dgram4 d;
        auto r = impl::do_create(*d.impl_, bind_addr, config);

        if (!r)
            return r.error();

        return d;
    }

    dgram4 &dgram4::on_data(data_handler handler)
    {
        impl_->on_data_cb = std::move(handler);
        impl_->arm();
        return *this;
    }

    dgram4 &dgram4::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    void dgram4::pause()
    {
        impl_->do_pause();
    }

    void dgram4::resume()
    {
        impl_->do_resume();
    }

    bool dgram4::paused() const noexcept
    {
        return impl_->paused.load(std::memory_order_relaxed);
    }

    bool dgram4::send_to(const void *data, size_t len, const inet4_address &dest)
    {
        return impl_->do_send_to(data, len, dest);
    }

    inet4_address dgram4::local_addr() const
    {
        return impl_->do_local_addr();
    }

    // ── dgram6 ───────────────────────────────────────────────────────────────────

    struct dgram6::impl : dgram_impl<dgram6, udp6_socket, inet6_address>
    {
    };

    dgram6::dgram6(): impl_(std::make_unique<impl>()) {}

    dgram6::~dgram6()
    {
        if (impl_)
            impl_->do_destroy();
    }

    dgram6::dgram6(dgram6 &&) noexcept = default;
    dgram6 &dgram6::operator=(dgram6 &&) noexcept = default;

    result<dgram6> dgram6::create(inet6_address bind_addr, dgram_config config)
    {
        dgram6 d;
        auto r = impl::do_create(*d.impl_, bind_addr, config);

        if (!r)
            return r.error();

        return d;
    }

    dgram6 &dgram6::on_data(data_handler handler)
    {
        impl_->on_data_cb = std::move(handler);
        impl_->arm();
        return *this;
    }

    dgram6 &dgram6::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    void dgram6::pause()
    {
        impl_->do_pause();
    }

    void dgram6::resume()
    {
        impl_->do_resume();
    }

    bool dgram6::paused() const noexcept
    {
        return impl_->paused.load(std::memory_order_relaxed);
    }

    bool dgram6::send_to(const void *data, size_t len, const inet6_address &dest)
    {
        return impl_->do_send_to(data, len, dest);
    }

    inet6_address dgram6::local_addr() const
    {
        return impl_->do_local_addr();
    }

} // namespace socketpp

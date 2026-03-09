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
 * - Batch send/recv for high-throughput scenarios
 *
 * Unlike the old udp_server, the factory creates the object fully initialized
 * (socket open, event loop running) and the destructor tears it all down.
 * There is no start()/stop()/run().
 */

#include "../platform/detect_internal.hpp"
#include "dgram_peer_impl.hpp"
#include "serial_queue.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cstring>
#include <future>
#include <socketpp/dgram.hpp>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/udp.hpp>
#include <socketpp/socket/udp_peer.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

namespace socketpp
{

    // ── dgram impl template ──────────────────────────────────────────────────────

    namespace
    {

        template<
            typename DgramType,
            typename PeerType,
            typename Socket,
            typename PeerSocket,
            typename Address,
            typename SendEntry,
            typename Message>
        struct dgram_impl
        {
            using peer_impl_type = detail::dgram_peer_impl<PeerType, Address, Socket, PeerSocket, SendEntry>;

            event_loop loop;
            std::unique_ptr<detail::thread_pool> pool;
            std::unique_ptr<detail::serial_queue> serial; ///< Serializes user callbacks for this dgram.
            Socket socket;

            typename DgramType::data_handler on_data_cb;
            typename DgramType::batch_data_handler on_data_batch_cb;
            typename DgramType::error_handler on_error_cb;

            std::vector<char> read_buf;

            // Batch recv state
            std::vector<char> batch_arena;
            std::vector<msg_batch_entry> batch_entries;
            size_t batch_size = 0;
            size_t buf_size = 0;

            std::thread background_thread;
            std::atomic<bool> armed {false};
            std::atomic<bool> paused {false};

            /// Claimed peer map -- accessed only on the event loop thread.
            std::unordered_map<Address, std::shared_ptr<peer_impl_type>> claimed_peers;

            /// Event loop background thread id, stored during do_create() for
            /// same-thread detection in do_claim().
            std::thread::id loop_thread_id;

            /// Factory initialization: open socket, apply SO flags, bind, start loop.
            static result<void> do_create(dgram_impl &self, const Address &addr, const dgram_config &config)
            {
                auto effective_opts = config.sock_opts;
#if defined(SOCKETPP_OS_WINDOWS)
                if (!effective_opts.has_reuse_addr())
                    effective_opts.exclusive_addr(true);
#endif

#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
                // Connected UDP peer sockets (claim path) bind to the same
                // local address:port as the parent. Both sockets must have
                // SO_REUSEADDR + SO_REUSEPORT for the kernel to allow this
                // and route by 4-tuple. macOS enforces this strictly; Linux
                // is more permissive but setting both is the correct pattern.
                if (!effective_opts.has_reuse_addr())
                    effective_opts.reuse_addr(true);
                effective_opts.reuse_port(true);
#endif

                auto r = self.socket.open(addr, effective_opts);

                if (!r)
                    return r;

                self.socket.set_non_blocking(true);

                self.pool = std::make_unique<detail::thread_pool>(config.worker_threads);
                self.serial = std::make_unique<detail::serial_queue>(self.pool.get());
                self.buf_size = config.read_buffer_size;
                self.read_buf.resize(config.read_buffer_size);

                // Allocate batch recv arena and entries
                self.batch_size = config.recv_batch_size;
                self.batch_arena.resize(self.batch_size * config.read_buffer_size);
                self.batch_entries.resize(self.batch_size);

                for (size_t i = 0; i < self.batch_size; ++i)
                {
                    self.batch_entries[i].buf = self.batch_arena.data() + i * config.read_buffer_size;
                    self.batch_entries[i].len = config.read_buffer_size;
                    self.batch_entries[i].transferred = 0;
                }

                // Start the event loop on a background thread. The socket is NOT
                // registered for reading yet -- that happens in arm().
                self.background_thread = std::thread(
                    [&self]()
                    {
                        self.loop_thread_id = std::this_thread::get_id();
                        self.loop.run();
                    });

                return {};
            }

            /// Handles a readable event — drains via recv_batch, dispatches to callbacks.
            void handle_readable()
            {
                constexpr size_t max_iterations = 8; // 8 * batch_size datagrams max

                for (size_t iter = 0; iter < max_iterations; ++iter)
                {
                    // Reset entry lengths for this batch
                    for (size_t i = 0; i < batch_size; ++i)
                    {
                        batch_entries[i].len = buf_size;
                        batch_entries[i].transferred = 0;
                    }

                    auto recv_r = socket.recv_batch(span<msg_batch_entry>(batch_entries));

                    if (!recv_r)
                    {
                        if (recv_r.error() == make_error_code(errc::would_block))
                            break;

                        if (on_error_cb)
                        {
                            auto ec = recv_r.error();
                            serial->submit([this, ec]() { on_error_cb(ec); });
                        }

                        break;
                    }

                    auto count = recv_r.value();

                    if (count == 0)
                        break;

                    if (on_data_batch_cb)
                    {
                        // Copy received data into a shared buffer and build message array.
                        // Claimed peers are checked per-entry; claimed datagrams are
                        // forwarded to the peer and excluded from the batch callback.
                        size_t total_bytes = 0;
                        int unclaimed_count = 0;

                        for (int i = 0; i < count; ++i)
                        {
                            Address from;
                            std::memcpy(&from, batch_entries[i].addr.data(), sizeof(from));

                            auto it = claimed_peers.find(from);

                            if (it != claimed_peers.end())
                            {
                                // Lazy cleanup: if peer was relinquished, remove and
                                // let datagram fall through to main callback.
                                if (!it->second->open_.load(std::memory_order_relaxed))
                                {
                                    claimed_peers.erase(it);
                                    total_bytes += batch_entries[i].transferred;
                                    ++unclaimed_count;
                                    continue;
                                }

                                // Forward to claimed peer. On Linux/macOS this is a race
                                // straggler (kernel already routing to connected socket);
                                // on Windows this is the primary delivery path.
                                if constexpr (!has_kernel_udp_demux)
                                {
                                    it->second->deliver_data(
                                        static_cast<const char *>(batch_entries[i].buf), batch_entries[i].transferred);
                                }
                                // Either way, skip main callback for this datagram.
                            }
                            else
                            {
                                total_bytes += batch_entries[i].transferred;
                                ++unclaimed_count;
                            }
                        }

                        if (unclaimed_count > 0)
                        {
                            auto shared_buf = std::make_shared<std::vector<char>>(total_bytes);
                            auto shared_msgs = std::make_shared<std::vector<Message>>(unclaimed_count);

                            size_t offset = 0;
                            int msg_idx = 0;

                            for (int i = 0; i < count; ++i)
                            {
                                Address from;
                                std::memcpy(&from, batch_entries[i].addr.data(), sizeof(from));

                                auto cp_it = claimed_peers.find(from);
                                if (cp_it != claimed_peers.end()
                                    && cp_it->second->open_.load(std::memory_order_relaxed))
                                    continue;

                                auto &entry = batch_entries[i];
                                std::memcpy(shared_buf->data() + offset, entry.buf, entry.transferred);

                                (*shared_msgs)[msg_idx].data = shared_buf->data() + offset;
                                (*shared_msgs)[msg_idx].len = entry.transferred;
                                (*shared_msgs)[msg_idx].from = from;

                                offset += entry.transferred;
                                ++msg_idx;
                            }

                            serial->submit(
                                [this, shared_buf, shared_msgs]()
                                { on_data_batch_cb(span<const Message>(shared_msgs->data(), shared_msgs->size())); });
                        }
                    }
                    else if (on_data_cb)
                    {
                        for (int i = 0; i < count; ++i)
                        {
                            auto &entry = batch_entries[i];

                            Address sender;
                            std::memcpy(&sender, entry.addr.data(), sizeof(sender));

                            // Check claimed peers before dispatching to main on_data.
                            auto it = claimed_peers.find(sender);

                            if (it != claimed_peers.end())
                            {
                                // Lazy cleanup: if peer was relinquished, remove and
                                // let datagram fall through to main callback.
                                if (!it->second->open_.load(std::memory_order_relaxed))
                                {
                                    claimed_peers.erase(it);
                                    // Fall through to main on_data below.
                                }
                                else
                                {
                                    // On Windows: forward to claimed peer.
                                    // On Linux/macOS: straggler from race at claim time; drop.
                                    if constexpr (!has_kernel_udp_demux)
                                    {
                                        it->second->deliver_data(
                                            static_cast<const char *>(entry.buf), entry.transferred);
                                    }

                                    continue;
                                }
                            }

                            auto data = std::vector<char>(
                                static_cast<char *>(entry.buf), static_cast<char *>(entry.buf) + entry.transferred);

                            serial->submit([this, d = std::move(data), sender]()
                                           { on_data_cb(d.data(), d.size(), sender); });
                        }
                    }

                    if (static_cast<size_t>(count) < batch_size)
                        break;
                }
            }

            /// Called by on_data() / on_data_batch() to register the socket for readable events.
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

                                handle_readable();
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

                                handle_readable();
                            });
                    });
            }

            /// Set on_data callback — clears on_data_batch (mutually exclusive).
            void do_on_data(typename DgramType::data_handler handler)
            {
                on_data_cb = std::move(handler);
                on_data_batch_cb = nullptr;
                arm();
            }

            /// Set on_data_batch callback — clears on_data (mutually exclusive).
            void do_on_data_batch(typename DgramType::batch_data_handler handler)
            {
                on_data_batch_cb = std::move(handler);
                on_data_cb = nullptr;
                arm();
            }

            /// Synchronous send -- called directly from the user's thread.
            bool do_send_to(const void *data, size_t len, const Address &dest)
            {
                auto r = socket.send_to(data, len, dest);
                return !!r;
            }

            /// Synchronous batch send -- called directly from the user's thread.
            result<int> do_send_batch(span<const SendEntry> msgs)
            {
                // Stack-alloc up to 64 entries, heap beyond
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
                    sock_address sa = msgs[i].dest;
                    entries[i].buf = const_cast<void *>(msgs[i].data);
                    entries[i].len = msgs[i].len;
                    entries[i].addr = sa;
                    entries[i].transferred = 0;
                }

                return socket.send_batch(span<msg_batch_entry>(entries, msgs.size()));
            }

            /// Get the local bound address.
            Address do_local_addr() const
            {
                auto r = socket.local_addr();
                if (!r)
                    return {};
                return r.value();
            }

            /// Schedule a one-shot timer via the event loop; callback fires on thread pool.
            timer_handle do_defer(std::chrono::milliseconds delay, std::function<void()> cb)
            {
                auto p = std::make_shared<std::promise<timer_handle>>();
                auto f = p->get_future();
                auto *sq = serial.get();

                loop.post(
                    [this, delay, cb = std::move(cb), p, sq]() mutable
                    {
                        auto h = loop.defer(delay, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                        p->set_value(std::move(h));
                    });

                return f.get();
            }

            /// Schedule a repeating timer via the event loop; callback fires on thread pool.
            timer_handle do_repeat(std::chrono::milliseconds interval, std::function<void()> cb)
            {
                auto p = std::make_shared<std::promise<timer_handle>>();
                auto f = p->get_future();
                auto *sq = serial.get();

                loop.post(
                    [this, interval, cb = std::move(cb), p, sq]() mutable
                    {
                        auto h = loop.repeat(interval, [sq, cb = std::move(cb)]() { sq->submit(cb); });
                        p->set_value(std::move(h));
                    });

                return f.get();
            }

            /// Post a callback through the event loop to the thread pool.
            void do_post(std::function<void()> cb)
            {
                auto *sq = serial.get();
                loop.post([sq, cb = std::move(cb)]() { sq->submit(cb); });
            }

            /// Claim a peer address: creates a peer impl, inserts into claimed_peers map.
            ///
            /// On Linux/macOS: also creates a connected UDP peer socket for kernel demux.
            /// On Windows: no peer socket; parent's handle_readable() routes traffic.
            ///
            /// Deadlock prevention: if called from the event loop thread (e.g. inside
            /// an on_data callback), the insertion is executed inline instead of posting
            /// to the event loop and blocking on a future.
            /// Claim a peer address using a pre-created peer impl.
            ///
            /// The caller creates the shared_ptr<peer_impl_type> (or a derived type)
            /// and passes it in. This avoids the base-to-derived shared_ptr conversion
            /// issue: dgram4::claim() creates shared_ptr<dgram4_peer::impl> (derived)
            /// and passes it here as shared_ptr<peer_impl_type> (base). The caller
            /// retains the derived shared_ptr for assignment to dgram4_peer::impl_.
            result<void> do_claim(const Address &peer_addr, std::shared_ptr<peer_impl_type> pi)
            {
                auto execute_claim = [this, &peer_addr, &pi]() -> result<void>
                {
                    if (claimed_peers.find(peer_addr) != claimed_peers.end())
                        return errc::address_in_use;

                    if constexpr (has_kernel_udp_demux)
                    {
                        auto local = do_local_addr();
                        auto peer_r = PeerSocket::create(local, peer_addr);

                        if (!peer_r)
                            return peer_r.error();

                        pi->peer_socket_ = std::move(peer_r.value());
                    }

                    claimed_peers[peer_addr] = pi;
                    return {};
                };

                // Same-thread detection: if we're on the event loop thread,
                // execute inline to avoid deadlock from post+future.
                if (std::this_thread::get_id() == loop_thread_id)
                {
                    auto r = execute_claim();

                    if (!r)
                        return r.error();

                    if constexpr (has_kernel_udp_demux)
                        pi->arm(buf_size);

                    return {};
                }
                else
                {
                    auto p = std::make_shared<std::promise<result<void>>>();
                    auto f = p->get_future();

                    loop.post([p, execute_claim]() { p->set_value(execute_claim()); });

                    auto r = f.get();

                    if (!r)
                        return r.error();

                    if constexpr (has_kernel_udp_demux)
                        pi->arm(buf_size);

                    return {};
                }
            }

            /// Relinquish a previously claimed peer. Removes from claimed_peers map
            /// and cleans up the peer impl (closes peer socket on Linux/macOS).
            void do_relinquish(const Address &peer_addr)
            {
                // Same-thread detection for deadlock prevention
                if (std::this_thread::get_id() == loop_thread_id)
                {
                    auto it = claimed_peers.find(peer_addr);

                    if (it != claimed_peers.end())
                    {
                        it->second->do_relinquish();
                        claimed_peers.erase(it);
                    }
                }
                else
                {
                    auto p = std::make_shared<std::promise<void>>();
                    auto f = p->get_future();

                    loop.post(
                        [this, peer_addr, p]()
                        {
                            auto it = claimed_peers.find(peer_addr);

                            if (it != claimed_peers.end())
                            {
                                it->second->do_relinquish();
                                claimed_peers.erase(it);
                            }

                            p->set_value();
                        });

                    f.get();
                }
            }

            /// Shutdown sequence: relinquish all peers, stop loop, join thread,
            /// close socket, shutdown pool.
            ///
            /// Peers must be relinquished BEFORE stopping the event loop because
            /// on Linux/macOS, relinquish removes peer socket fds from the event
            /// loop's dispatcher. Those removals must execute while the loop is
            /// still running.
            ///
            /// Uses post() to send the stop signal INSIDE the event loop. This
            /// avoids a race where loop.stop() is called before run() starts
            /// (run() resets stop_requested_ to false, losing a direct stop()).
            /// The posted callback executes after run()'s reset, guaranteeing
            /// the stop takes effect. The direct stop() call handles the edge
            /// case where the loop hasn't started polling yet.
            void do_destroy()
            {
                // Relinquish all claimed peers inline (we haven't stopped the
                // loop yet, so fd removal posts will execute).
                loop.post(
                    [this]()
                    {
                        for (auto &[addr, pi] : claimed_peers)
                            pi->do_relinquish();

                        claimed_peers.clear();
                    });

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

    struct dgram4::impl :
        dgram_impl<dgram4, dgram4_peer, udp4_socket, udp4_peer_socket, inet4_address, dgram4_send_entry, dgram4_message>
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
        impl_->do_on_data(std::move(handler));
        return *this;
    }

    dgram4 &dgram4::on_data_batch(batch_data_handler handler)
    {
        impl_->do_on_data_batch(std::move(handler));
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

    result<int> dgram4::send_batch(span<const dgram4_send_entry> msgs)
    {
        return impl_->do_send_batch(msgs);
    }

    result<dgram4_peer> dgram4::claim(const inet4_address &peer_addr)
    {
        auto pi = std::make_shared<dgram4_peer::impl>(peer_addr, &impl_->loop, impl_->pool.get(), &impl_->socket);
        auto r = impl_->do_claim(peer_addr, pi);

        if (!r)
            return r.error();

        dgram4_peer peer;
        peer.impl_ = std::move(pi);
        return peer;
    }

    timer_handle dgram4::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle dgram4::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void dgram4::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

    inet4_address dgram4::local_addr() const
    {
        return impl_->do_local_addr();
    }

    // ── dgram6 ───────────────────────────────────────────────────────────────────

    struct dgram6::impl :
        dgram_impl<dgram6, dgram6_peer, udp6_socket, udp6_peer_socket, inet6_address, dgram6_send_entry, dgram6_message>
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
        impl_->do_on_data(std::move(handler));
        return *this;
    }

    dgram6 &dgram6::on_data_batch(batch_data_handler handler)
    {
        impl_->do_on_data_batch(std::move(handler));
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

    result<int> dgram6::send_batch(span<const dgram6_send_entry> msgs)
    {
        return impl_->do_send_batch(msgs);
    }

    result<dgram6_peer> dgram6::claim(const inet6_address &peer_addr)
    {
        auto pi = std::make_shared<dgram6_peer::impl>(peer_addr, &impl_->loop, impl_->pool.get(), &impl_->socket);
        auto r = impl_->do_claim(peer_addr, pi);

        if (!r)
            return r.error();

        dgram6_peer peer;
        peer.impl_ = std::move(pi);
        return peer;
    }

    timer_handle dgram6::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle dgram6::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void dgram6::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

    inet6_address dgram6::local_addr() const
    {
        return impl_->do_local_addr();
    }

} // namespace socketpp

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
 * @file connection_engine.hpp
 * @brief Core TCP connection engine template (IPv4 and IPv6).
 *
 * Contains the tcp_conn_impl template that provides all connection logic:
 * non-blocking reads, buffered writes, event loop integration, and thread-safe
 * close semantics. stream_conn_impl (in stream_connection_impl.hpp) extends
 * this with pause/resume flow control.
 *
 * This is an internal header -- not part of the public API.
 *
 * Key design notes:
 * - The impl inherits from enable_shared_from_this so it can safely capture
 *   `self` in lambdas posted to the event loop and thread pool.
 * - All I/O callbacks run on the event loop thread. User-facing callbacks
 *   (on_data, on_close, on_error) are dispatched to the thread pool via
 *   pool_->submit().
 * - The write path is lock-free on the event loop side: enqueue_send() holds
 *   write_mutex_ briefly to push data and post a writable-interest request.
 *   drain_write_queue() runs on the event loop and only locks briefly to
 *   dequeue chunks.
 * - close is idempotent via atomic exchange on closed_.
 */

#ifndef SOCKETPP_DETAIL_CONNECTION_ENGINE_HPP
#define SOCKETPP_DETAIL_CONNECTION_ENGINE_HPP

#include "serial_queue.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <socketpp/event/loop.hpp>
#include <socketpp/socket/tcp.hpp>
#include <vector>

namespace socketpp::detail
{

    /**
     * @brief Core connection implementation, templated on socket and address type.
     *
     * Manages the lifecycle of a single TCP connection: reading data from the
     * socket, buffering outbound writes, and coordinating close across threads.
     *
     * @tparam Socket  The low-level socket type (tcp4_socket or tcp6_socket).
     * @tparam Address The address type (inet4_address or inet6_address).
     */
    template<typename Socket, typename Address>
    struct tcp_conn_impl : std::enable_shared_from_this<tcp_conn_impl<Socket, Address>>
    {
        using data_handler = std::function<void(const char *, size_t)>;
        using close_handler = std::function<void()>;
        using error_handler = std::function<void(std::error_code)>;

        Socket socket_; ///< The underlying non-blocking TCP socket.
        Address peer_; ///< Remote peer address, captured at accept/connect time.
        Address local_; ///< Local address, captured at accept/connect time.
        event_loop *loop_; ///< The event loop driving this connection's I/O.
        thread_pool *pool_; ///< Thread pool for dispatching user callbacks.
        serial_queue serial_; ///< Serializes user callbacks for this connection.

        std::vector<char> read_buf_; ///< Per-connection read buffer.

        std::mutex write_mutex_; ///< Protects write_queue_, write_queue_bytes_, write_registered_.
        std::deque<std::vector<char>> write_queue_; ///< FIFO queue of outbound data chunks.
        size_t write_queue_bytes_ = 0; ///< Total bytes across all queued chunks.
        size_t max_write_queue_bytes_; ///< Backpressure limit; send() fails if exceeded.
        bool write_registered_ = false; ///< Whether writable interest has been requested.

        /// @warning Handlers must only be set before start_reading() is called
        ///          (i.e. during on_connect). The post() spinlock in start_reading()
        ///          provides the happens-before relationship that makes the handler
        ///          writes visible to the event loop thread. Setting handlers after
        ///          start_reading() is a data race on the std::function objects.
        data_handler on_data_; ///< User callback for received data.
        close_handler on_close_; ///< User callback for connection close.
        error_handler on_error_; ///< User callback for errors.

        std::atomic<bool> closed_ {false}; ///< Ensures close logic runs at most once.

        tcp_conn_impl(
            Socket &&sock,
            Address peer,
            Address local,
            event_loop &loop,
            thread_pool &pool,
            size_t read_buf_size,
            size_t max_write_bytes):
            socket_(std::move(sock)),
            peer_(peer),
            local_(local),
            loop_(&loop),
            pool_(&pool),
            serial_(&pool),
            max_write_queue_bytes_(max_write_bytes)
        {
            read_buf_.resize(read_buf_size);
        }

        /**
         * @brief Register the socket with the event loop and begin reading.
         *
         * Posts a task to the event loop that adds the socket to the I/O
         * dispatcher with readable interest. If send() was called before this
         * method (e.g. during on_connect), writable interest is also included
         * so the queued data gets flushed.
         *
         * This two-phase design (construct impl, then start_reading) exists
         * because the on_connect callback runs between construction and
         * registration. The user may call send() during on_connect, which
         * queues data but cannot register writable interest because the
         * socket is not yet in the dispatcher. start_reading() bridges this
         * gap by checking the write queue.
         */
        void start_reading()
        {
            auto self = this->shared_from_this();

            loop_->post(
                [self]()
                {
                    auto fd = self->socket_.native_handle();

                    // Check if send() was called before start_reading (e.g. during on_connect).
                    // If so, data is queued but register_writable() could not run because the
                    // socket wasn't in the dispatcher yet. Include writable interest now so the
                    // queued data is flushed on the first writable notification.
                    auto interest = io_event::readable;
                    {
                        std::lock_guard<std::mutex> lock(self->write_mutex_);
                        if (!self->write_queue_.empty())
                            interest |= io_event::writable;
                    }

                    self->loop_->io().add(
                        fd, interest, [self](socket_t s, io_event events) { self->on_io_event(s, events); });
                });
        }

        /**
         * @brief Central I/O event handler, called on the event loop thread.
         *
         * Dispatches to the appropriate path based on the event flags:
         * - error/hangup: initiate close
         * - readable: recv and dispatch on_data to thread pool
         * - writable: drain the write queue
         */
        void on_io_event(socket_t /*fd*/, io_event events)
        {
            if (closed_.load(std::memory_order_relaxed))
                return;

            if (has_event(events, io_event::error) || has_event(events, io_event::hangup))
            {
                initiate_close();
                return;
            }

            if (has_event(events, io_event::readable))
            {
                auto r = socket_.recv(read_buf_.data(), read_buf_.size());

                if (!r || r.value() == 0)
                {
                    // would_block is not a real error -- just means no data available yet.
                    if (!r && r.error() == make_error_code(errc::would_block))
                        return;

                    // recv returning 0 means graceful peer shutdown; errors mean something broke.
                    // Either way, close the connection.
                    initiate_close();
                    return;
                }

                auto n = r.value();

                if (on_data_)
                {
                    // Copy into a standalone buffer so the user callback can process it
                    // without racing against the next recv() on the event loop.
                    auto data = std::vector<char>(read_buf_.data(), read_buf_.data() + n);
                    auto self = this->shared_from_this();

                    serial_.submit([self, d = std::move(data)]() { self->on_data_(d.data(), d.size()); });
                }
            }

            if (has_event(events, io_event::writable))
            {
                drain_write_queue();
            }
        }

        /**
         * @brief Flush queued write data to the socket.
         *
         * Runs on the event loop thread. Sends chunks from the write queue
         * until the queue is empty or the socket would block. When the queue
         * empties, writable interest is removed to avoid busy-spinning.
         *
         * If a partial send occurs (would_block mid-chunk), the unsent
         * remainder is pushed back to the front of the queue for the next
         * writable notification.
         */
        void drain_write_queue()
        {
            for (;;)
            {
                std::vector<char> chunk;

                {
                    std::lock_guard<std::mutex> lock(write_mutex_);

                    if (write_queue_.empty())
                    {
                        // All data flushed. Remove writable interest to stop receiving
                        // writable notifications (which would busy-spin).
                        write_registered_ = false;

                        loop_->io().modify(socket_.native_handle(), io_event::readable);

                        return;
                    }

                    chunk = std::move(write_queue_.front());
                    write_queue_.pop_front();
                    write_queue_bytes_ -= chunk.size();
                }

                size_t sent = 0;

                while (sent < chunk.size())
                {
                    auto r = socket_.send(chunk.data() + sent, chunk.size() - sent);

                    if (!r)
                    {
                        if (r.error() == make_error_code(errc::would_block))
                        {
                            // Socket buffer full. Push the unsent remainder back to the
                            // front of the queue. The next writable event will resume.
                            std::lock_guard<std::mutex> lock(write_mutex_);
                            auto remaining = std::vector<char>(chunk.begin() + sent, chunk.end());
                            write_queue_bytes_ += remaining.size();
                            write_queue_.push_front(std::move(remaining));
                            return;
                        }

                        // Fatal send error -- close the connection.
                        initiate_close();
                        return;
                    }

                    sent += r.value();
                }
            }
        }

        /**
         * @brief Queue data for sending (thread-safe).
         *
         * Called from any thread (typically a user thread via connection::send()).
         * Copies the data into the write queue under a lock, then posts a
         * register_writable() task to the event loop if writable interest has
         * not already been requested.
         *
         * @param data Pointer to the data to send.
         * @param len  Number of bytes.
         * @return true if queued, false if closed, len is 0, or backpressure
         *         limit would be exceeded.
         */
        bool enqueue_send(const void *data, size_t len)
        {
            if (closed_.load(std::memory_order_relaxed) || len == 0)
                return false;

            std::lock_guard<std::mutex> lock(write_mutex_);

            // Backpressure: reject if this send would exceed the configured limit.
            if (write_queue_bytes_ + len > max_write_queue_bytes_)
                return false;

            write_queue_.emplace_back(static_cast<const char *>(data), static_cast<const char *>(data) + len);
            write_queue_bytes_ += len;

            if (!write_registered_)
            {
                write_registered_ = true;
                auto self = this->shared_from_this();
                // Post to the event loop to modify interest flags. This is necessary
                // because the I/O dispatcher must only be modified from the event
                // loop thread on some backends.
                loop_->post([self]() { self->register_writable(); });
            }

            return true;
        }

        /**
         * @brief Add writable interest to the socket's event registration.
         *
         * Runs on the event loop thread (posted by enqueue_send).
         */
        void register_writable()
        {
            if (closed_.load(std::memory_order_relaxed))
                return;

            loop_->io().modify(socket_.native_handle(), io_event::readable | io_event::writable);
        }

        /**
         * @brief Close the connection from the event loop thread.
         *
         * Idempotent via atomic exchange on closed_. Removes the socket from
         * the I/O dispatcher, closes the underlying socket, and dispatches
         * the on_close callback to the thread pool.
         *
         * Called from on_io_event (error/hangup/recv-zero/send-error) or
         * from close_from_user() via an event loop post.
         */
        void initiate_close()
        {
            // Atomic exchange ensures this body runs at most once, even if
            // close is triggered concurrently from multiple paths.
            if (closed_.exchange(true, std::memory_order_acq_rel))
                return;

            auto fd = socket_.native_handle();

            loop_->io().remove(fd);
            socket_.close();

            if (on_close_)
            {
                auto self = this->shared_from_this();
                serial_.submit([self]() { self->on_close_(); });
            }
        }

        /**
         * @brief Request close from a user thread.
         *
         * Posts initiate_close() to the event loop so the actual close runs
         * on the event loop thread, avoiding races with I/O processing.
         */
        void close_from_user()
        {
            if (closed_.load(std::memory_order_relaxed))
                return;

            auto self = this->shared_from_this();
            loop_->post([self]() { self->initiate_close(); });
        }
    };

} // namespace socketpp::detail

#endif // SOCKETPP_DETAIL_CONNECTION_ENGINE_HPP

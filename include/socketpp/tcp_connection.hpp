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
 * @file tcp_connection.hpp
 * @brief High-level TCP connection abstractions for IPv4 and IPv6.
 *
 * Provides tcp4_connection and tcp6_connection, which represent individual
 * TCP connections managed by a tcp_server or tcp_client. Connections are
 * non-copyable, move-only handles backed by a shared internal implementation.
 *
 * All user-facing callbacks (on_data, on_close, on_error) are dispatched on
 * a thread pool -- never on the event loop thread. This means callbacks may
 * execute concurrently with each other and with calls to send().
 *
 * @note The connection's internal lifetime is managed via shared_ptr. The
 *       server/client keeps a shared_ptr to the impl, so the connection
 *       remains alive as long as it is tracked. When the last reference to
 *       the impl is dropped and the connection is still open, the destructor
 *       will initiate an asynchronous close.
 *
 * @warning Capturing a connection by reference (`&conn`) in a callback is
 *          safe only while the connection is alive. The server/client stores
 *          a shared_ptr<Connection> wrapper to guarantee this. If you store
 *          your own reference, ensure it does not outlive the connection.
 */

#ifndef SOCKETPP_TCP_CONNECTION_HPP
#define SOCKETPP_TCP_CONNECTION_HPP

#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <string>
#include <system_error>

namespace socketpp
{

    /**
     * @brief A handle to an established IPv4 TCP connection.
     *
     * Obtained from tcp4_server's on_connect callback or tcp4_client after
     * a successful connection. Non-copyable; move-only. The underlying socket
     * I/O is fully non-blocking and driven by the event loop.
     *
     * @note It is safe to call send() from within the on_connect callback.
     *       Data will be queued and flushed once the socket is registered
     *       with the event loop dispatcher (start_reading() checks the write
     *       queue and includes writable interest if data is already queued).
     *
     * @warning All callbacks (on_data, on_close, on_error) run on the thread
     *          pool, not the event loop thread. Do not block the event loop.
     */
    class tcp4_connection
    {
      public:
        /** @brief Callback invoked when data is received. Parameters are a pointer to the data and its length. */
        using data_handler = std::function<void(const char *, size_t)>;

        /** @brief Callback invoked when the connection is closed (locally or by the peer). */
        using close_handler = std::function<void()>;

        /** @brief Callback invoked when an error occurs on the connection. */
        using error_handler = std::function<void(std::error_code)>;

        /**
         * @brief Destructor.
         *
         * If this is the last handle to the connection and it is still open,
         * an asynchronous close is initiated via the event loop.
         */
        ~tcp4_connection();

        tcp4_connection(const tcp4_connection &) = delete;
        tcp4_connection &operator=(const tcp4_connection &) = delete;

        /** @brief Move constructor. Transfers ownership of the underlying connection. */
        tcp4_connection(tcp4_connection &&) noexcept;

        /** @brief Move assignment. Transfers ownership of the underlying connection. */
        tcp4_connection &operator=(tcp4_connection &&) noexcept;

        /**
         * @brief Register a callback for incoming data.
         * @param handler Called on the thread pool with (data, length) for each read.
         *                The data pointer is only valid for the duration of the call.
         */
        void on_data(data_handler handler);

        /**
         * @brief Register a callback for connection close.
         * @param handler Called on the thread pool when the connection closes.
         *                Called at most once.
         */
        void on_close(close_handler handler);

        /**
         * @brief Register a callback for connection errors.
         * @param handler Called on the thread pool with the error code.
         */
        void on_error(error_handler handler);

        /**
         * @brief Queue data for sending.
         *
         * Thread-safe. Data is copied into an internal write queue and flushed
         * asynchronously by the event loop when the socket becomes writable.
         *
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @return true if the data was queued successfully, false if the
         *         connection is closed, len is 0, or the write queue would
         *         exceed the configured max_write_buffer.
         *
         * @note Safe to call from on_connect before the event loop has
         *       registered the socket. Queued data will be picked up when
         *       start_reading() adds writable interest.
         */
        bool send(const void *data, size_t len);

        /**
         * @brief Queue a string for sending.
         * @param data The string to send.
         * @return true if the data was queued successfully.
         * @see send(const void*, size_t)
         */
        bool send(const std::string &data);

        /**
         * @brief Initiate an asynchronous close of the connection.
         *
         * The actual close happens on the event loop thread. The on_close
         * callback will fire once the close completes. Calling close() on
         * an already-closed connection is a no-op.
         */
        void close();

        /**
         * @brief Get the remote peer address.
         * @return The IPv4 address and port of the connected peer.
         */
        inet4_address peer_addr() const;

        /**
         * @brief Get the local address of this connection.
         * @return The local IPv4 address and port.
         */
        inet4_address local_addr() const;

        /**
         * @brief Check whether the connection is still open.
         * @return true if the connection has not been closed.
         */
        bool is_open() const noexcept;

        /**
         * @brief Get the number of bytes currently queued for writing.
         * @return Byte count of pending outbound data.
         * @note Thread-safe; acquires the write mutex internally.
         */
        size_t write_queue_bytes() const noexcept;

        /** @cond INTERNAL */
        struct impl;
        std::shared_ptr<impl> impl_;
        explicit tcp4_connection(std::shared_ptr<impl> p);
        /** @endcond */

      private:
        friend class tcp4_server;
        friend class tcp4_client;
    };

    /**
     * @brief A handle to an established IPv6 TCP connection.
     *
     * IPv6 counterpart to tcp4_connection. All semantics, callbacks, and
     * thread-safety guarantees are identical; only the address type differs.
     *
     * @see tcp4_connection for full documentation of behavior and caveats.
     */
    class tcp6_connection
    {
      public:
        /** @brief Callback invoked when data is received. Parameters are a pointer to the data and its length. */
        using data_handler = std::function<void(const char *, size_t)>;

        /** @brief Callback invoked when the connection is closed (locally or by the peer). */
        using close_handler = std::function<void()>;

        /** @brief Callback invoked when an error occurs on the connection. */
        using error_handler = std::function<void(std::error_code)>;

        /**
         * @brief Destructor.
         * @see tcp4_connection::~tcp4_connection()
         */
        ~tcp6_connection();

        tcp6_connection(const tcp6_connection &) = delete;
        tcp6_connection &operator=(const tcp6_connection &) = delete;

        /** @brief Move constructor. Transfers ownership of the underlying connection. */
        tcp6_connection(tcp6_connection &&) noexcept;

        /** @brief Move assignment. Transfers ownership of the underlying connection. */
        tcp6_connection &operator=(tcp6_connection &&) noexcept;

        /**
         * @brief Register a callback for incoming data.
         * @param handler Called on the thread pool with (data, length) for each read.
         */
        void on_data(data_handler handler);

        /**
         * @brief Register a callback for connection close.
         * @param handler Called on the thread pool when the connection closes.
         */
        void on_close(close_handler handler);

        /**
         * @brief Register a callback for connection errors.
         * @param handler Called on the thread pool with the error code.
         */
        void on_error(error_handler handler);

        /**
         * @brief Queue data for sending.
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @return true if the data was queued successfully.
         * @see tcp4_connection::send(const void*, size_t)
         */
        bool send(const void *data, size_t len);

        /**
         * @brief Queue a string for sending.
         * @param data The string to send.
         * @return true if the data was queued successfully.
         */
        bool send(const std::string &data);

        /**
         * @brief Initiate an asynchronous close of the connection.
         * @see tcp4_connection::close()
         */
        void close();

        /**
         * @brief Get the remote peer address.
         * @return The IPv6 address and port of the connected peer.
         */
        inet6_address peer_addr() const;

        /**
         * @brief Get the local address of this connection.
         * @return The local IPv6 address and port.
         */
        inet6_address local_addr() const;

        /**
         * @brief Check whether the connection is still open.
         * @return true if the connection has not been closed.
         */
        bool is_open() const noexcept;

        /**
         * @brief Get the number of bytes currently queued for writing.
         * @return Byte count of pending outbound data.
         */
        size_t write_queue_bytes() const noexcept;

        /** @cond INTERNAL */
        struct impl;
        std::shared_ptr<impl> impl_;
        explicit tcp6_connection(std::shared_ptr<impl> p);
        /** @endcond */

      private:
        friend class tcp6_server;
        friend class tcp6_client;
    };

} // namespace socketpp

#endif // SOCKETPP_TCP_CONNECTION_HPP

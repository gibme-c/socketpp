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
 * @file tcp_client.hpp
 * @brief High-level TCP client abstractions for IPv4 and IPv6.
 *
 * Provides tcp4_client and tcp6_client, which perform asynchronous outbound
 * TCP connections and manage the resulting connection's lifetime. User
 * callbacks are dispatched on a thread pool -- never on the event loop thread.
 *
 * Typical usage:
 * @code
 *   socketpp::tcp4_client client;
 *   client
 *       .connect_timeout(std::chrono::seconds(5))
 *       .on_connect([](socketpp::tcp4_connection &conn) {
 *           conn.send("GET / HTTP/1.0\r\n\r\n");  // safe in on_connect
 *           conn.on_data([](const char *data, size_t len) {
 *               // handle response
 *           });
 *       })
 *       .on_error([](std::error_code ec) {
 *           // handle connection failure
 *       });
 *
 *   client.connect({"127.0.0.1", 80});
 *   client.run();  // blocks until stop()
 * @endcode
 *
 * @note The client stores a shared_ptr<Connection> wrapper internally to keep
 *       the connection alive for user callbacks that capture `&conn`.
 *
 * @warning stop() joins the event loop background thread BEFORE touching
 *          connection state. This ordering prevents data races.
 */

#ifndef SOCKETPP_TCP_CLIENT_HPP
#define SOCKETPP_TCP_CLIENT_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/tcp_connection.hpp>

namespace socketpp
{

    /**
     * @brief High-level IPv4 TCP client.
     *
     * Manages an asynchronous outbound connection, an event loop, and a thread
     * pool. All configuration methods return `*this` for chaining and must be
     * called before connect().
     */
    class tcp4_client
    {
      public:
        /**
         * @brief Callback invoked when the connection is established.
         * @note Runs on the thread pool, not the event loop thread.
         * @note It is safe to call conn.send() inside this callback.
         */
        using connect_handler = std::function<void(tcp4_connection &)>;

        /** @brief Callback invoked when a connection error occurs. Runs on the thread pool. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a client with default settings. */
        tcp4_client();

        /** @brief Destructor. Calls stop() if the client was started. */
        ~tcp4_client();

        tcp4_client(const tcp4_client &) = delete;
        tcp4_client &operator=(const tcp4_client &) = delete;

        /** @brief Move constructor. */
        tcp4_client(tcp4_client &&) noexcept;

        /** @brief Move assignment. */
        tcp4_client &operator=(tcp4_client &&) noexcept;

        /**
         * @brief Set the handler called when the connection is established.
         * @param handler The connect callback.
         * @return Reference to this client for chaining.
         */
        tcp4_client &on_connect(connect_handler handler);

        /**
         * @brief Set the handler called on connection errors.
         * @param handler The error callback.
         * @return Reference to this client for chaining.
         */
        tcp4_client &on_error(error_handler handler);

        /**
         * @brief Set the number of worker threads in the thread pool.
         * @param count Number of threads. Pass 0 for auto-detect (minimum 2).
         * @return Reference to this client for chaining.
         */
        tcp4_client &worker_threads(size_t count);

        /**
         * @brief Set the maximum write buffer size for the connection.
         * @param bytes Maximum bytes that can be queued before send() returns
         *              false. Defaults to 16 MB.
         * @return Reference to this client for chaining.
         */
        tcp4_client &max_write_buffer(size_t bytes);

        /**
         * @brief Set the read buffer size for the connection.
         * @param bytes Size of the read buffer. Defaults to 64 KB.
         * @return Reference to this client for chaining.
         */
        tcp4_client &read_buffer_size(size_t bytes);

        /**
         * @brief Set socket options applied to the outbound socket.
         * @param opts Socket options. Defaults to tcp_nodelay(true).
         * @return Reference to this client for chaining.
         */
        tcp4_client &socket_opts(const socket_options &opts);

        /**
         * @brief Set the connection timeout.
         * @param ms Timeout in milliseconds. Defaults to 30 seconds. If the
         *           connection is not established within this period, the
         *           on_error callback fires.
         * @return Reference to this client for chaining.
         */
        tcp4_client &connect_timeout(std::chrono::milliseconds ms);

        /**
         * @brief Initiate an asynchronous connection to the given address.
         *
         * The connection proceeds asynchronously. Call run() or start() after
         * this to drive the event loop.
         *
         * @param addr The remote address and port to connect to.
         */
        void connect(const inet4_address &addr);

        /**
         * @brief Run the event loop on the calling thread (blocking).
         *
         * Blocks until stop() is called. Call connect() first.
         */
        void run();

        /**
         * @brief Start the event loop on a background thread (non-blocking).
         *
         * Returns immediately. Call connect() first. Use stop() to shut down.
         */
        void start();

        /**
         * @brief Stop the client and clean up the connection.
         *
         * Stops the event loop, joins the background thread (if any),
         * closes the connection, and shuts down the thread pool.
         */
        void stop();

        /**
         * @brief Check whether the client is currently connected.
         * @return true if a connection is established and has not been closed.
         */
        bool connected() const noexcept;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    /**
     * @brief High-level IPv6 TCP client.
     *
     * IPv6 counterpart to tcp4_client. All semantics, callbacks, and
     * thread-safety guarantees are identical; only the address type differs.
     *
     * @see tcp4_client for full documentation of behavior and caveats.
     */
    class tcp6_client
    {
      public:
        /**
         * @brief Callback invoked when the connection is established.
         * @see tcp4_client::connect_handler
         */
        using connect_handler = std::function<void(tcp6_connection &)>;

        /** @brief Callback invoked when a connection error occurs. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a client with default settings. */
        tcp6_client();

        /** @brief Destructor. Calls stop() if the client was started. */
        ~tcp6_client();

        tcp6_client(const tcp6_client &) = delete;
        tcp6_client &operator=(const tcp6_client &) = delete;
        tcp6_client(tcp6_client &&) noexcept;
        tcp6_client &operator=(tcp6_client &&) noexcept;

        /** @brief Set the handler called when the connection is established. */
        tcp6_client &on_connect(connect_handler handler);

        /** @brief Set the handler called on connection errors. */
        tcp6_client &on_error(error_handler handler);

        /** @brief Set the number of worker threads. Pass 0 for auto-detect. */
        tcp6_client &worker_threads(size_t count);

        /** @brief Set the maximum write buffer size (default 16 MB). */
        tcp6_client &max_write_buffer(size_t bytes);

        /** @brief Set the read buffer size (default 64 KB). */
        tcp6_client &read_buffer_size(size_t bytes);

        /** @brief Set socket options for the outbound socket. */
        tcp6_client &socket_opts(const socket_options &opts);

        /** @brief Set the connection timeout (default 30 seconds). */
        tcp6_client &connect_timeout(std::chrono::milliseconds ms);

        /**
         * @brief Initiate an asynchronous connection to the given address.
         * @param addr The remote IPv6 address and port to connect to.
         */
        void connect(const inet6_address &addr);

        /** @brief Run the event loop on the calling thread (blocking). */
        void run();

        /** @brief Start the event loop on a background thread (non-blocking). */
        void start();

        /** @brief Stop the client and clean up the connection. */
        void stop();

        /** @brief Check whether the client is currently connected. */
        bool connected() const noexcept;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_TCP_CLIENT_HPP

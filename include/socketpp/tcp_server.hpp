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
 * @file tcp_server.hpp
 * @brief High-level TCP server abstractions for IPv4 and IPv6.
 *
 * Provides tcp4_server and tcp6_server, which accept incoming TCP connections,
 * manage their lifetimes, and dispatch user callbacks on a thread pool.
 *
 * Typical usage:
 * @code
 *   socketpp::tcp4_server server;
 *   server
 *       .on_connect([](socketpp::tcp4_connection &conn) {
 *           conn.on_data([](const char *data, size_t len) {
 *               // handle incoming data
 *           });
 *           conn.send("hello");  // safe to call here; data is queued
 *       })
 *       .on_error([](std::error_code ec) {
 *           // handle accept errors
 *       });
 *
 *   auto r = server.listen({"0.0.0.0", 8080});
 *   if (!r) { return; }
 *   server.run();  // blocks until stop() is called
 * @endcode
 *
 * @note The server stores a shared_ptr<Connection> wrapper for each accepted
 *       connection. This keeps the connection alive for user callbacks that
 *       capture `&conn`. When a connection closes, the wrapper is erased from
 *       the connections map on the event loop thread.
 *
 * @warning Stop ordering matters: stop() joins the event loop background
 *          thread BEFORE iterating the connections map. This prevents races
 *          between the event loop modifying the map and the shutdown path
 *          reading it.
 */

#ifndef SOCKETPP_TCP_SERVER_HPP
#define SOCKETPP_TCP_SERVER_HPP

#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/tcp_connection.hpp>

namespace socketpp
{

    /**
     * @brief High-level IPv4 TCP server.
     *
     * Manages a listening socket, an event loop, and a thread pool. Accepts
     * incoming connections and delivers them to the user via the on_connect
     * callback. All configuration methods return `*this` for chaining and
     * must be called before listen().
     *
     * The server owns each connection's lifetime. Connections are automatically
     * removed from the internal map when they close.
     */
    class tcp4_server
    {
      public:
        /**
         * @brief Callback invoked when a new client connects.
         * @note Runs on the thread pool, not the event loop thread.
         * @note It is safe to call conn.send() inside this callback.
         *       The data will be queued and flushed once start_reading()
         *       registers the socket with writable interest.
         */
        using connect_handler = std::function<void(tcp4_connection &)>;

        /** @brief Callback invoked when an accept-level error occurs. Runs on the thread pool. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a server with default settings. */
        tcp4_server();

        /**
         * @brief Destructor. Calls stop() if the server is running.
         */
        ~tcp4_server();

        tcp4_server(const tcp4_server &) = delete;
        tcp4_server &operator=(const tcp4_server &) = delete;

        /** @brief Move constructor. */
        tcp4_server(tcp4_server &&) noexcept;

        /** @brief Move assignment. */
        tcp4_server &operator=(tcp4_server &&) noexcept;

        /**
         * @brief Set the handler called when a new connection is accepted.
         * @param handler The connect callback.
         * @return Reference to this server for chaining.
         */
        tcp4_server &on_connect(connect_handler handler);

        /**
         * @brief Set the handler called on accept errors.
         * @param handler The error callback.
         * @return Reference to this server for chaining.
         */
        tcp4_server &on_error(error_handler handler);

        /**
         * @brief Set the number of worker threads in the thread pool.
         * @param count Number of threads. Pass 0 to use hardware_concurrency
         *              (minimum 2).
         * @return Reference to this server for chaining.
         */
        tcp4_server &worker_threads(size_t count);

        /**
         * @brief Set the maximum write buffer size per connection.
         * @param bytes Maximum bytes that can be queued for writing on a
         *              single connection before send() returns false.
         *              Defaults to 16 MB.
         * @return Reference to this server for chaining.
         */
        tcp4_server &max_write_buffer(size_t bytes);

        /**
         * @brief Set the read buffer size per connection.
         * @param bytes Size of the per-connection read buffer. Defaults to 64 KB.
         * @return Reference to this server for chaining.
         */
        tcp4_server &read_buffer_size(size_t bytes);

        /**
         * @brief Set socket options applied to the listening socket.
         * @param opts Socket options. Defaults to reuse_addr(true) and tcp_nodelay(true).
         * @return Reference to this server for chaining.
         */
        tcp4_server &socket_opts(const socket_options &opts);

        /**
         * @brief Set the listen backlog.
         * @param value Backlog size for the listening socket.
         * @return Reference to this server for chaining.
         */
        tcp4_server &backlog(int value);

        /**
         * @brief Set the maximum number of concurrent connections.
         *
         * When the limit is reached, newly accepted sockets are immediately
         * closed. A value of 0 means unlimited (the default).
         *
         * @param count Maximum concurrent connections allowed.
         * @return Reference to this server for chaining.
         */
        tcp4_server &max_connections(size_t count);

        /**
         * @brief Bind and start listening on the given address.
         *
         * Must be called after configuration and before run() or start().
         *
         * @param addr The address and port to listen on.
         * @return result<void> indicating success or the error that occurred.
         */
        result<void> listen(const inet4_address &addr);

        /**
         * @brief Run the event loop on the calling thread (blocking).
         *
         * Blocks until stop() is called from another thread or a signal handler.
         * Call listen() first.
         */
        void run();

        /**
         * @brief Start the event loop on a background thread (non-blocking).
         *
         * Returns immediately. Use stop() to shut down.
         * Call listen() first.
         */
        void start();

        /**
         * @brief Stop the server and clean up all connections.
         *
         * Stops the event loop, joins the background thread (if any),
         * closes all connections, and shuts down the thread pool.
         *
         * @warning The event loop thread is joined BEFORE iterating the
         *          connections map. This ordering is critical to avoid
         *          data races between the event loop and the shutdown path.
         */
        void stop();

        /**
         * @brief Check whether the event loop is currently running.
         * @return true if the event loop is active.
         */
        bool running() const noexcept;

        /**
         * @brief Get the current number of tracked connections.
         * @return Number of active connections.
         * @note Thread-safe. May be called from any thread at any time.
         */
        size_t connection_count() const noexcept;

        /**
         * @brief Get the address the server is listening on.
         * @return The bound address, including the actual port if 0 was used.
         */
        inet4_address listening_address() const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    /**
     * @brief High-level IPv6 TCP server.
     *
     * IPv6 counterpart to tcp4_server. All semantics, callbacks, and
     * thread-safety guarantees are identical; only the address type differs.
     *
     * @see tcp4_server for full documentation of behavior and caveats.
     */
    class tcp6_server
    {
      public:
        /**
         * @brief Callback invoked when a new client connects.
         * @see tcp4_server::connect_handler
         */
        using connect_handler = std::function<void(tcp6_connection &)>;

        /** @brief Callback invoked when an accept-level error occurs. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a server with default settings. */
        tcp6_server();

        /** @brief Destructor. Calls stop() if the server is running. */
        ~tcp6_server();

        tcp6_server(const tcp6_server &) = delete;
        tcp6_server &operator=(const tcp6_server &) = delete;
        tcp6_server(tcp6_server &&) noexcept;
        tcp6_server &operator=(tcp6_server &&) noexcept;

        /** @brief Set the handler called when a new connection is accepted. */
        tcp6_server &on_connect(connect_handler handler);

        /** @brief Set the handler called on accept errors. */
        tcp6_server &on_error(error_handler handler);

        /** @brief Set the number of worker threads. Pass 0 for auto-detect. */
        tcp6_server &worker_threads(size_t count);

        /** @brief Set the maximum write buffer size per connection (default 16 MB). */
        tcp6_server &max_write_buffer(size_t bytes);

        /** @brief Set the read buffer size per connection (default 64 KB). */
        tcp6_server &read_buffer_size(size_t bytes);

        /** @brief Set socket options for the listening socket. */
        tcp6_server &socket_opts(const socket_options &opts);

        /** @brief Set the listen backlog. */
        tcp6_server &backlog(int value);

        /**
         * @brief Set the maximum number of concurrent connections.
         * @param count Maximum concurrent connections allowed. 0 = unlimited.
         * @return Reference to this server for chaining.
         * @see tcp4_server::max_connections()
         */
        tcp6_server &max_connections(size_t count);

        /**
         * @brief Bind and start listening on the given address.
         * @param addr The IPv6 address and port to listen on.
         * @return result<void> indicating success or the error that occurred.
         */
        result<void> listen(const inet6_address &addr);

        /** @brief Run the event loop on the calling thread (blocking). */
        void run();

        /** @brief Start the event loop on a background thread (non-blocking). */
        void start();

        /** @brief Stop the server and clean up all connections. */
        void stop();

        /** @brief Check whether the event loop is currently running. */
        bool running() const noexcept;

        /** @brief Get the current number of tracked connections. */
        size_t connection_count() const noexcept;

        /** @brief Get the address the server is listening on. */
        inet6_address listening_address() const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_TCP_SERVER_HPP

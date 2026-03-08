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
 * @file udp_server.hpp
 * @brief High-level UDP server abstractions for IPv4 and IPv6.
 *
 * Provides udp4_server and udp6_server, which bind a UDP socket and deliver
 * incoming datagrams to user callbacks dispatched on a thread pool.
 *
 * Unlike TCP servers, UDP servers are connectionless. Each received datagram
 * is delivered independently via the on_message callback along with the
 * sender's address. Replies can be sent with send_to().
 *
 * Typical usage:
 * @code
 *   socketpp::udp4_server server;
 *   server
 *       .on_message([&](const char *data, size_t len, const socketpp::inet4_address &sender) {
 *           server.send_to(data, len, sender);  // echo back
 *       })
 *       .on_error([](std::error_code ec) {
 *           // handle recv errors
 *       });
 *
 *   auto r = server.bind({"0.0.0.0", 9000});
 *   if (!r) { return; }
 *   server.run();
 * @endcode
 *
 * @note All callbacks (on_message, on_error) run on the thread pool,
 *       never on the event loop thread.
 *
 * @warning send_to() performs a synchronous send on the calling thread.
 *          It is not queued through the event loop. This is suitable for
 *          small reply datagrams but may block briefly if the socket buffer
 *          is full.
 */

#ifndef SOCKETPP_UDP_SERVER_HPP
#define SOCKETPP_UDP_SERVER_HPP

#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/socket/options.hpp>

namespace socketpp
{

    /**
     * @brief High-level IPv4 UDP server.
     *
     * Manages a bound UDP socket, an event loop, and a thread pool. Incoming
     * datagrams are read in a non-blocking loop and delivered to the user
     * via on_message. All configuration methods return `*this` for chaining
     * and must be called before bind().
     */
    class udp4_server
    {
      public:
        /**
         * @brief Callback invoked when a datagram is received.
         * @param data   Pointer to the received datagram payload.
         * @param len    Length of the received payload in bytes.
         * @param sender Address of the remote sender.
         * @note Runs on the thread pool, not the event loop thread.
         * @note The data pointer is only valid for the duration of the call.
         */
        using message_handler = std::function<void(const char *, size_t, const inet4_address &)>;

        /** @brief Callback invoked when a recv error occurs. Runs on the thread pool. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a server with default settings. */
        udp4_server();

        /** @brief Destructor. Calls stop() if the server is running. */
        ~udp4_server();

        udp4_server(const udp4_server &) = delete;
        udp4_server &operator=(const udp4_server &) = delete;

        /** @brief Move constructor. */
        udp4_server(udp4_server &&) noexcept;

        /** @brief Move assignment. */
        udp4_server &operator=(udp4_server &&) noexcept;

        /**
         * @brief Set the handler called when a datagram is received.
         * @param handler The message callback.
         * @return Reference to this server for chaining.
         */
        udp4_server &on_message(message_handler handler);

        /**
         * @brief Set the handler called on recv errors.
         * @param handler The error callback.
         * @return Reference to this server for chaining.
         */
        udp4_server &on_error(error_handler handler);

        /**
         * @brief Set the number of worker threads in the thread pool.
         * @param count Number of threads. Pass 0 for auto-detect (minimum 2).
         * @return Reference to this server for chaining.
         */
        udp4_server &worker_threads(size_t count);

        /**
         * @brief Set socket options applied to the UDP socket.
         * @param opts Socket options. Defaults to reuse_addr(true).
         * @return Reference to this server for chaining.
         */
        udp4_server &socket_opts(const socket_options &opts);

        /**
         * @brief Set the read buffer size.
         * @param bytes Size of the read buffer. Defaults to 64 KB. Should be
         *              at least as large as the largest expected datagram.
         * @return Reference to this server for chaining.
         */
        udp4_server &read_buffer_size(size_t bytes);

        /**
         * @brief Bind the UDP socket to the given address and configure the event loop.
         *
         * Must be called after configuration and before run() or start().
         *
         * @param addr The address and port to bind to.
         * @return result<void> indicating success or the error that occurred.
         */
        result<void> bind(const inet4_address &addr);

        /**
         * @brief Run the event loop on the calling thread (blocking).
         *
         * Blocks until stop() is called. Call bind() first.
         */
        void run();

        /**
         * @brief Start the event loop on a background thread (non-blocking).
         *
         * Returns immediately. Call bind() first. Use stop() to shut down.
         */
        void start();

        /**
         * @brief Stop the server and clean up resources.
         *
         * Stops the event loop, joins the background thread, closes the
         * socket, and shuts down the thread pool.
         */
        void stop();

        /**
         * @brief Send a datagram to the specified destination.
         *
         * Performs a synchronous sendto() on the calling thread.
         *
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @param dest Destination address and port.
         * @return true if the send succeeded, false on error.
         *
         * @warning This is a blocking call. It does not go through the
         *          event loop's write queue.
         */
        bool send_to(const void *data, size_t len, const inet4_address &dest);

        /**
         * @brief Check whether the event loop is currently running.
         * @return true if the event loop is active.
         */
        bool running() const noexcept;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    /**
     * @brief High-level IPv6 UDP server.
     *
     * IPv6 counterpart to udp4_server. All semantics, callbacks, and
     * thread-safety guarantees are identical; only the address type differs.
     *
     * @see udp4_server for full documentation of behavior and caveats.
     */
    class udp6_server
    {
      public:
        /**
         * @brief Callback invoked when a datagram is received.
         * @see udp4_server::message_handler
         */
        using message_handler = std::function<void(const char *, size_t, const inet6_address &)>;

        /** @brief Callback invoked when a recv error occurs. */
        using error_handler = std::function<void(std::error_code)>;

        /** @brief Construct a server with default settings. */
        udp6_server();

        /** @brief Destructor. Calls stop() if the server is running. */
        ~udp6_server();

        udp6_server(const udp6_server &) = delete;
        udp6_server &operator=(const udp6_server &) = delete;
        udp6_server(udp6_server &&) noexcept;
        udp6_server &operator=(udp6_server &&) noexcept;

        /** @brief Set the handler called when a datagram is received. */
        udp6_server &on_message(message_handler handler);

        /** @brief Set the handler called on recv errors. */
        udp6_server &on_error(error_handler handler);

        /** @brief Set the number of worker threads. Pass 0 for auto-detect. */
        udp6_server &worker_threads(size_t count);

        /** @brief Set socket options for the UDP socket. */
        udp6_server &socket_opts(const socket_options &opts);

        /** @brief Set the read buffer size (default 64 KB). */
        udp6_server &read_buffer_size(size_t bytes);

        /**
         * @brief Bind the UDP socket to the given address.
         * @param addr The IPv6 address and port to bind to.
         * @return result<void> indicating success or the error that occurred.
         */
        result<void> bind(const inet6_address &addr);

        /** @brief Run the event loop on the calling thread (blocking). */
        void run();

        /** @brief Start the event loop on a background thread (non-blocking). */
        void start();

        /** @brief Stop the server and clean up resources. */
        void stop();

        /**
         * @brief Send a datagram to the specified destination.
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @param dest Destination IPv6 address and port.
         * @return true if the send succeeded, false on error.
         */
        bool send_to(const void *data, size_t len, const inet6_address &dest);

        /** @brief Check whether the event loop is currently running. */
        bool running() const noexcept;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_UDP_SERVER_HPP

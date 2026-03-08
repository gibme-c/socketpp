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
 * @file stream.hpp
 * @brief High-level TCP stream abstractions for IPv4 and IPv6.
 *
 * Provides stream4 and stream6: unified TCP types supporting both server
 * (listen) and client (connect) roles via separate factory methods on a
 * single type. Connections are represented as nested types (stream4::connection).
 *
 * Lifecycle is RAII via factory pattern:
 * - stream4::listen(addr) opens a listener, starts the event loop
 * - stream4::connect(addr) initiates an outbound connection, starts the event loop
 * - on_connect() arms the accept loop or connect initiation
 * - destructor stops the loop, joins the thread, cleans up
 *
 * pause()/resume() flow control:
 * - On stream (listen mode): stops/resumes accepting new connections
 * - On connection: removes/restores readable interest (TCP flow control
 *   throttles the sender when the kernel buffer fills)
 */

#ifndef SOCKETPP_STREAM_HPP
#define SOCKETPP_STREAM_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>
#include <socketpp/socket/options.hpp>
#include <string>

namespace socketpp
{

    /**
     * @brief Configuration for stream listen (server) mode.
     */
    struct stream_listen_config
    {
        size_t worker_threads = 0;
        size_t max_write_buffer = 16 * 1024 * 1024;
        size_t read_buffer_size = 65536;
        socket_options sock_opts = socket_options {}.reuse_addr(true).tcp_nodelay(true);
        int backlog = default_backlog;
        size_t max_connections = 0;
    };

    /**
     * @brief Configuration for stream connect (client) mode.
     */
    struct stream_connect_config
    {
        size_t worker_threads = 0;
        size_t max_write_buffer = 16 * 1024 * 1024;
        size_t read_buffer_size = 65536;
        socket_options sock_opts = socket_options {}.tcp_nodelay(true);
        std::chrono::milliseconds connect_timeout {30000};
    };

    /**
     * @brief High-level IPv4 TCP stream type.
     *
     * Unified type for both listening (server) and connecting (client) TCP
     * operations. Created via static factory methods listen() or connect().
     * The event loop starts immediately but operations are not armed until
     * on_connect() is called, eliminating races between factory return and
     * callback setup.
     */
    class stream4
    {
      public:
        /**
         * @brief A handle to an established IPv4 TCP connection.
         *
         * Obtained from stream4's on_connect callback. Non-copyable; move-only.
         * The underlying socket I/O is fully non-blocking and driven by the
         * event loop.
         */
        class connection
        {
          public:
            struct impl; // forward-declared; defined internally

            using data_handler = std::function<void(const char *, size_t)>;
            using close_handler = std::function<void()>;
            using error_handler = std::function<void(std::error_code)>;

            void on_data(data_handler handler);
            void on_close(close_handler handler);
            void on_error(error_handler handler);

            bool send(const void *data, size_t len);
            bool send(const std::string &data);
            void close();

            void pause();
            void resume();
            bool paused() const noexcept;

            inet4_address peer_addr() const;
            inet4_address local_addr() const;
            bool is_open() const noexcept;
            size_t write_queue_bytes() const noexcept;

            ~connection();
            connection(connection &&) noexcept;
            connection &operator=(connection &&) noexcept;

            connection(const connection &) = delete;
            connection &operator=(const connection &) = delete;

            /// @cond internal
            std::shared_ptr<impl> impl_;
            /// @endcond

            /// @cond internal
            explicit connection(std::shared_ptr<impl> p);
            /// @endcond

          private:
            friend class stream4;
        };

        using connect_handler = std::function<void(connection &)>;
        using error_handler = std::function<void(std::error_code)>;

        /**
         * @brief Create a stream4 in listen (server) mode.
         *
         * Opens a listener socket, starts the event loop on a background
         * thread. The accept loop is NOT started until on_connect() is called.
         *
         * @param addr   Address and port to listen on.
         * @param config Listen configuration.
         * @return result<stream4> on success, or an error code.
         */
        static result<stream4> listen(inet4_address addr, stream_listen_config config = {});

        /**
         * @brief Create a stream4 in connect (client) mode.
         *
         * Starts the event loop on a background thread. The actual connection
         * is NOT initiated until on_connect() is called.
         *
         * @param addr   Remote address and port to connect to.
         * @param config Connect configuration.
         * @return result<stream4> on success, or an error code.
         */
        static result<stream4> connect(inet4_address addr, stream_connect_config config = {});

        /**
         * @brief Set the connect handler and arm the operation.
         *
         * In listen mode: starts the accept loop.
         * In connect mode: initiates the async connection.
         *
         * @param handler Called on the thread pool when a connection is established.
         * @return Reference to this instance for chaining.
         */
        stream4 &on_connect(connect_handler handler);

        /**
         * @brief Set the error handler.
         * @param handler Called on the thread pool when an error occurs.
         * @return Reference to this instance for chaining.
         */
        stream4 &on_error(error_handler handler);

        /**
         * @brief Pause the stream.
         *
         * In listen mode: stop accepting new connections (kernel backlog
         * buffers pending connections).
         * In connect mode: no-op.
         */
        void pause();

        /**
         * @brief Resume the stream.
         *
         * In listen mode: resume accepting connections.
         * In connect mode: no-op.
         */
        void resume();

        /** @brief Get the current number of tracked connections (listen mode). */
        size_t connection_count() const noexcept;

        /** @brief Get the local address. */
        inet4_address local_addr() const;

        /** @brief Destructor. Stops event loop, joins thread, cleans up. */
        ~stream4();

        /** @brief Move constructor. */
        stream4(stream4 &&) noexcept;

        /** @brief Move assignment. */
        stream4 &operator=(stream4 &&) noexcept;

        stream4(const stream4 &) = delete;
        stream4 &operator=(const stream4 &) = delete;

      private:
        stream4();
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    /**
     * @brief High-level IPv6 TCP stream type.
     *
     * IPv6 counterpart to stream4. All semantics, callbacks, and thread-safety
     * guarantees are identical; only the address type differs.
     *
     * @see stream4 for full documentation.
     */
    class stream6
    {
      public:
        class connection
        {
          public:
            struct impl; // forward-declared; defined internally

            using data_handler = std::function<void(const char *, size_t)>;
            using close_handler = std::function<void()>;
            using error_handler = std::function<void(std::error_code)>;

            void on_data(data_handler handler);
            void on_close(close_handler handler);
            void on_error(error_handler handler);

            bool send(const void *data, size_t len);
            bool send(const std::string &data);
            void close();

            void pause();
            void resume();
            bool paused() const noexcept;

            inet6_address peer_addr() const;
            inet6_address local_addr() const;
            bool is_open() const noexcept;
            size_t write_queue_bytes() const noexcept;

            ~connection();
            connection(connection &&) noexcept;
            connection &operator=(connection &&) noexcept;

            connection(const connection &) = delete;
            connection &operator=(const connection &) = delete;

            /// @cond internal
            std::shared_ptr<impl> impl_;
            /// @endcond

            /// @cond internal
            explicit connection(std::shared_ptr<impl> p);
            /// @endcond

          private:
            friend class stream6;
        };

        using connect_handler = std::function<void(connection &)>;
        using error_handler = std::function<void(std::error_code)>;

        static result<stream6> listen(inet6_address addr, stream_listen_config config = {});
        static result<stream6> connect(inet6_address addr, stream_connect_config config = {});

        stream6 &on_connect(connect_handler handler);
        stream6 &on_error(error_handler handler);

        void pause();
        void resume();

        size_t connection_count() const noexcept;
        inet6_address local_addr() const;

        ~stream6();
        stream6(stream6 &&) noexcept;
        stream6 &operator=(stream6 &&) noexcept;

        stream6(const stream6 &) = delete;
        stream6 &operator=(const stream6 &) = delete;

      private:
        stream6();
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_STREAM_HPP

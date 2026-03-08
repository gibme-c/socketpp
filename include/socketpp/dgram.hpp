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
 * @file dgram.hpp
 * @brief High-level UDP datagram abstractions for IPv4 and IPv6.
 *
 * Provides dgram4 and dgram6: unified datagram types with no client/server
 * role distinction. Role is a usage pattern: bind and receive (server), or
 * bind to ephemeral port and send (client) -- same socket, same type.
 *
 * Lifecycle is RAII via factory pattern:
 * - dgram4::create(bind_addr, config) opens the socket, starts the event loop
 * - on_data() arms readable interest (data delivery begins after this call)
 * - destructor stops the loop, joins the thread, closes the socket
 *
 * pause()/resume() control flow by removing/adding read interest from the
 * event loop. During pause, the kernel buffer absorbs data; for UDP, datagrams
 * are silently dropped when the kernel buffer fills (expected behavior).
 *
 * defer()/repeat() schedule one-shot and repeating timers on the event loop;
 * callbacks fire on the thread pool. post() dispatches a callback through the
 * event loop to the thread pool (useful for cross-thread wakeups).
 *
 * @note All callbacks (on_data, on_error, timer, post) run on the thread pool,
 *       never on the event loop thread.
 */

#ifndef SOCKETPP_DGRAM_HPP
#define SOCKETPP_DGRAM_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <socketpp/dgram_peer.hpp>
#include <socketpp/event/timer.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/socket/options.hpp>

namespace socketpp
{

    /**
     * @brief Configuration for dgram4/dgram6 instances.
     */
    struct dgram_config
    {
        size_t worker_threads = 0; ///< Thread pool size. 0 = auto-detect.
        socket_options sock_opts = socket_options {}.reuse_addr(true); ///< Socket options applied before bind.
        size_t read_buffer_size = 65536; ///< Read buffer size (>= largest expected datagram).
        size_t recv_batch_size = 32; ///< Max datagrams per recv_batch() call.
    };

    // ── Batch Send/Recv Types ─────────────────────────────────────────────────

    /** @brief Entry for batch send on dgram4. */
    struct dgram4_send_entry
    {
        const void *data;
        size_t len;
        inet4_address dest;
    };

    /** @brief Entry for batch send on dgram6. */
    struct dgram6_send_entry
    {
        const void *data;
        size_t len;
        inet6_address dest;
    };

    /** @brief Received datagram delivered to batch callback on dgram4. */
    struct dgram4_message
    {
        const char *data;
        size_t len;
        inet4_address from;
    };

    /** @brief Received datagram delivered to batch callback on dgram6. */
    struct dgram6_message
    {
        const char *data;
        size_t len;
        inet6_address from;
    };

    /**
     * @brief High-level IPv4 UDP datagram type.
     *
     * Unified type for both sending and receiving UDP datagrams over IPv4.
     * Created via the static factory method create(). The event loop starts
     * immediately but the socket is not registered for reading until on_data()
     * is called, eliminating the race between factory return and callback setup.
     */
    class dgram4
    {
      public:
        /** @brief Callback invoked when a datagram is received. Runs on the thread pool. */
        using data_handler = std::function<void(const char *, size_t, const inet4_address &)>;

        /** @brief Callback invoked when a batch of datagrams is received. Runs on the thread pool. */
        using batch_data_handler = std::function<void(span<const dgram4_message>)>;

        /** @brief Callback invoked when a recv error occurs. Runs on the thread pool. */
        using error_handler = std::function<void(std::error_code)>;

        /**
         * @brief Create a dgram4 instance bound to the given address.
         *
         * Opens the socket, applies SO flags, binds, starts the event loop
         * on a background thread. The socket is NOT registered for reading
         * until on_data() is called.
         *
         * @param bind_addr Address and port to bind to. Port 0 = ephemeral.
         * @param config    Configuration options.
         * @return result<dgram4> on success, or an error code on failure.
         */
        static result<dgram4> create(inet4_address bind_addr = inet4_address::any(0), dgram_config config = {});

        /**
         * @brief Set the data handler and arm readable interest.
         *
         * Once called, the socket is registered with the event loop for
         * readable events. Incoming datagrams are delivered to the handler
         * on the thread pool.
         *
         * Mutually exclusive with on_data_batch() — setting one clears the other.
         *
         * @param handler The data callback.
         * @return Reference to this instance for chaining.
         */
        dgram4 &on_data(data_handler handler);

        /**
         * @brief Set the batch data handler and arm readable interest.
         *
         * Incoming datagrams are delivered in batches to the handler on the
         * thread pool. Mutually exclusive with on_data() — setting one clears
         * the other.
         *
         * @param handler The batch data callback.
         * @return Reference to this instance for chaining.
         */
        dgram4 &on_data_batch(batch_data_handler handler);

        /**
         * @brief Set the error handler.
         * @param handler The error callback.
         * @return Reference to this instance for chaining.
         */
        dgram4 &on_error(error_handler handler);

        /**
         * @brief Pause reading from the socket.
         *
         * Removes readable interest from the event loop. The kernel buffer
         * absorbs datagrams during pause; datagrams are silently dropped if
         * the kernel buffer fills (expected UDP behavior).
         */
        void pause();

        /**
         * @brief Resume reading from the socket.
         *
         * Re-registers readable interest with the event loop. Datagrams
         * buffered in the kernel are drained immediately.
         */
        void resume();

        /**
         * @brief Check if the socket is currently paused.
         * @return true if pause() has been called and resume() has not.
         */
        bool paused() const noexcept;

        /**
         * @brief Send a datagram to the specified destination.
         *
         * Performs a synchronous sendto() on the calling thread.
         *
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @param dest Destination address and port.
         * @return true if the send succeeded, false on error.
         */
        bool send_to(const void *data, size_t len, const inet4_address &dest);

        /**
         * @brief Send multiple datagrams in a single batch.
         *
         * Performs a synchronous batch send on the calling thread.
         *
         * @param msgs Span of send entries describing each datagram.
         * @return Number of messages sent, or an error code.
         */
        result<int> send_batch(span<const dgram4_send_entry> msgs);

        /**
         * @brief Claim traffic from a specific peer address.
         *
         * Returns a dgram4_peer handle that captures all datagrams from the
         * given peer. The parent's on_data callback will no longer fire for
         * this peer. Each peer handle has its own serialized execution queue.
         *
         * On Linux/macOS: creates a connected UDP socket for kernel 4-tuple demux.
         * On Windows: routes traffic in-process from the parent's recv loop.
         *
         * Thread-safe: may be called from any thread, including on_data callbacks.
         *
         * @param peer_addr The peer address to claim.
         * @return result<dgram4_peer> on success, or errc::address_in_use if
         *         the peer is already claimed.
         */
        result<dgram4_peer> claim(const inet4_address &peer_addr);

        /**
         * @brief Schedule a one-shot timer.
         *
         * The callback fires once on the thread pool after the given delay.
         * Thread-safe: may be called from any thread.
         *
         * @param delay  Time until the callback fires.
         * @param cb     Callback to invoke.
         * @return A handle that can cancel the timer.
         */
        timer_handle defer(std::chrono::milliseconds delay, std::function<void()> cb);

        /**
         * @brief Schedule a repeating timer.
         *
         * The callback fires on the thread pool at the given interval until
         * cancelled. Thread-safe: may be called from any thread.
         *
         * @param interval Time between successive firings.
         * @param cb       Callback to invoke.
         * @return A handle that can cancel the timer.
         */
        timer_handle repeat(std::chrono::milliseconds interval, std::function<void()> cb);

        /**
         * @brief Post a callback for execution on the thread pool.
         *
         * The callback is dispatched through the event loop to the thread pool.
         * Thread-safe: may be called from any thread.
         *
         * @param cb Callback to invoke.
         */
        void post(std::function<void()> cb);

        /**
         * @brief Get the local address the socket is bound to.
         * @return The bound address, including the actual port if 0 was used.
         */
        inet4_address local_addr() const;

        /** @brief Destructor. Stops the event loop, joins the thread, closes the socket. */
        ~dgram4();

        /** @brief Move constructor. */
        dgram4(dgram4 &&) noexcept;

        /** @brief Move assignment. */
        dgram4 &operator=(dgram4 &&) noexcept;

        dgram4(const dgram4 &) = delete;
        dgram4 &operator=(const dgram4 &) = delete;

      private:
        dgram4();
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    /**
     * @brief High-level IPv6 UDP datagram type.
     *
     * IPv6 counterpart to dgram4. All semantics, callbacks, and thread-safety
     * guarantees are identical; only the address type differs.
     *
     * @see dgram4 for full documentation of behavior and caveats.
     */
    class dgram6
    {
      public:
        /** @brief Callback invoked when a datagram is received. */
        using data_handler = std::function<void(const char *, size_t, const inet6_address &)>;

        /** @brief Callback invoked when a batch of datagrams is received. */
        using batch_data_handler = std::function<void(span<const dgram6_message>)>;

        /** @brief Callback invoked when a recv error occurs. */
        using error_handler = std::function<void(std::error_code)>;

        /**
         * @brief Create a dgram6 instance bound to the given address.
         * @param bind_addr IPv6 address and port to bind to. Port 0 = ephemeral.
         * @param config    Configuration options.
         * @return result<dgram6> on success, or an error code on failure.
         */
        static result<dgram6> create(inet6_address bind_addr = inet6_address::any(0), dgram_config config = {});

        /** @brief Set the data handler and arm readable interest. Mutually exclusive with on_data_batch(). */
        dgram6 &on_data(data_handler handler);

        /** @brief Set the batch data handler and arm readable interest. Mutually exclusive with on_data(). */
        dgram6 &on_data_batch(batch_data_handler handler);

        /** @brief Set the error handler. */
        dgram6 &on_error(error_handler handler);

        /** @brief Pause reading from the socket. */
        void pause();

        /** @brief Resume reading from the socket. */
        void resume();

        /** @brief Check if the socket is currently paused. */
        bool paused() const noexcept;

        /**
         * @brief Send a datagram to the specified destination.
         * @param data Pointer to the data to send.
         * @param len  Number of bytes to send.
         * @param dest Destination IPv6 address and port.
         * @return true if the send succeeded, false on error.
         */
        bool send_to(const void *data, size_t len, const inet6_address &dest);

        /**
         * @brief Send multiple datagrams in a single batch.
         * @param msgs Span of send entries describing each datagram.
         * @return Number of messages sent, or an error code.
         */
        result<int> send_batch(span<const dgram6_send_entry> msgs);

        /**
         * @brief Claim traffic from a specific peer address.
         * @see dgram4::claim for full documentation.
         */
        result<dgram6_peer> claim(const inet6_address &peer_addr);

        /** @brief Schedule a one-shot timer. @see dgram4::defer */
        timer_handle defer(std::chrono::milliseconds delay, std::function<void()> cb);

        /** @brief Schedule a repeating timer. @see dgram4::repeat */
        timer_handle repeat(std::chrono::milliseconds interval, std::function<void()> cb);

        /** @brief Post a callback for execution on the thread pool. @see dgram4::post */
        void post(std::function<void()> cb);

        /** @brief Get the local address the socket is bound to. */
        inet6_address local_addr() const;

        /** @brief Destructor. Stops the event loop, joins the thread, closes the socket. */
        ~dgram6();

        /** @brief Move constructor. */
        dgram6(dgram6 &&) noexcept;

        /** @brief Move assignment. */
        dgram6 &operator=(dgram6 &&) noexcept;

        dgram6(const dgram6 &) = delete;
        dgram6 &operator=(const dgram6 &) = delete;

      private:
        dgram6();
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace socketpp

#endif // SOCKETPP_DGRAM_HPP

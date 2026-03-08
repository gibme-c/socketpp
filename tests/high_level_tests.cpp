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

/// @file high_level_tests.cpp
/// Tests for the high-level server/client API.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <socketpp.hpp>
#include <string>
#include <thread>
#include <vector>

static int g_fail_count = 0;
static int g_test_count = 0;

#define CHECK(expr)                                                                           \
    do                                                                                        \
    {                                                                                         \
        ++g_test_count;                                                                       \
        if (!(expr))                                                                          \
        {                                                                                     \
            std::cerr << "  FAIL: " << #expr << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_fail_count;                                                                   \
        }                                                                                     \
    } while (0)

#define CHECK_MSG(expr, msg)                                                                                  \
    do                                                                                                        \
    {                                                                                                         \
        ++g_test_count;                                                                                       \
        if (!(expr))                                                                                          \
        {                                                                                                     \
            std::cerr << "  FAIL: " << #expr << " - " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_fail_count;                                                                                   \
        }                                                                                                     \
    } while (0)

#define RUN_TEST(fn)                        \
    do                                      \
    {                                       \
        int fail_before = g_fail_count;     \
        std::cerr << "  " << #fn << "... "; \
        fn();                               \
        if (g_fail_count == fail_before)    \
            std::cerr << "ok\n";            \
        else                                \
            std::cerr << "FAILED\n";        \
    } while (0)

namespace
{

    void platform_sleep_ms(int ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    bool wait_for(std::atomic<bool> &flag, int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (!flag.load(std::memory_order_relaxed))
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return true;
    }

    bool wait_for_count(std::atomic<int> &counter, int target, int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (counter.load(std::memory_order_relaxed) < target)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return true;
    }

} // namespace

// ===========================================================================
// Thread pool tests
// ===========================================================================

void test_thread_pool_submit_and_execute()
{
    // Thread pool is internal, but we can verify it works via the server:
    // callbacks must run on pool threads (not the main thread).
    const uint16_t port = 19874;
    std::atomic<bool> callback_ran {false};
    auto main_tid = std::this_thread::get_id();
    bool ran_on_different_thread = false;

    socketpp::tcp4_server server;
    server.on_connect(
        [&](socketpp::tcp4_connection &conn)
        {
            ran_on_different_thread = (std::this_thread::get_id() != main_tid);
            callback_ran.store(true);
            conn.on_data([](const char *, size_t) {});
        });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    socketpp::tcp4_client client;
    client.on_connect([](socketpp::tcp4_connection &) {});
    client.connect(socketpp::inet4_address::loopback(port));
    client.start();

    wait_for(callback_ran);
    CHECK(callback_ran.load());
    CHECK(ran_on_different_thread);

    client.stop();
    server.stop();
}

// ===========================================================================
// TCP echo round trip
// ===========================================================================

void test_tcp_echo_round_trip()
{
    const uint16_t port = 19876;

    socketpp::tcp4_server server;
    std::atomic<bool> echo_received {false};
    std::string echo_data;
    std::mutex echo_mutex;

    server.on_connect([](socketpp::tcp4_connection &conn)
                      { conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); }); });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    socketpp::tcp4_client client;

    client.on_connect(
        [&echo_received, &echo_data, &echo_mutex](socketpp::tcp4_connection &conn)
        {
            conn.on_data(
                [&echo_received, &echo_data, &echo_mutex](const char *data, size_t len)
                {
                    std::lock_guard<std::mutex> lock(echo_mutex);
                    echo_data.assign(data, len);
                    echo_received.store(true, std::memory_order_relaxed);
                });

            conn.send("Hello, high-level API!");
        });

    client.connect(socketpp::inet4_address::loopback(port));
    client.start();

    auto ok = wait_for(echo_received);
    CHECK_MSG(ok, "echo response not received within timeout");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(echo_mutex);
        CHECK(echo_data == "Hello, high-level API!");
    }

    client.stop();
    server.stop();
}

// ===========================================================================
// TCP multiple clients
// ===========================================================================

void test_tcp_multiple_clients()
{
    const uint16_t port = 19877;
    const int num_clients = 5;

    socketpp::tcp4_server server;
    std::atomic<int> connect_count {0};

    server.on_connect(
        [&connect_count](socketpp::tcp4_connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
        });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    std::atomic<int> echo_count {0};
    std::vector<std::unique_ptr<socketpp::tcp4_client>> clients;

    for (int i = 0; i < num_clients; ++i)
    {
        auto client = std::make_unique<socketpp::tcp4_client>();

        client->on_connect(
            [&echo_count, i](socketpp::tcp4_connection &conn)
            {
                conn.on_data([&echo_count](const char *, size_t)
                             { echo_count.fetch_add(1, std::memory_order_relaxed); });

                std::string msg = "client-" + std::to_string(i);
                conn.send(msg);
            });

        client->connect(socketpp::inet4_address::loopback(port));
        client->start();
        clients.push_back(std::move(client));
        platform_sleep_ms(20);
    }

    auto ok = wait_for_count(echo_count, num_clients, 10000);
    CHECK_MSG(ok, "not all echoes received");
    CHECK(connect_count.load() >= num_clients);

    for (auto &c : clients)
        c->stop();

    server.stop();
}

// ===========================================================================
// TCP back-pressure (write queue full)
// ===========================================================================

void test_tcp_back_pressure()
{
    const uint16_t port = 19878;
    const size_t max_write = 4096;

    socketpp::tcp4_server server;
    std::atomic<bool> connected {false};
    socketpp::tcp4_connection *server_conn_ptr = nullptr;
    std::mutex conn_mutex;

    server.max_write_buffer(max_write);

    server.on_connect(
        [&connected, &server_conn_ptr, &conn_mutex](socketpp::tcp4_connection &conn)
        {
            {
                std::lock_guard<std::mutex> lock(conn_mutex);
                server_conn_ptr = &conn;
            }
            connected.store(true, std::memory_order_relaxed);

            // Don't read data — this creates back-pressure on the write side
            // when echoing back
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });

            // Keep the callback alive long enough
            platform_sleep_ms(2000);
        });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    socketpp::tcp4_client client;
    std::atomic<bool> client_connected {false};
    socketpp::tcp4_connection *client_conn_ptr = nullptr;
    std::mutex client_conn_mutex;

    client.max_write_buffer(max_write);

    client.on_connect(
        [&client_connected, &client_conn_ptr, &client_conn_mutex](socketpp::tcp4_connection &conn)
        {
            {
                std::lock_guard<std::mutex> lock(client_conn_mutex);
                client_conn_ptr = &conn;
            }
            client_connected.store(true, std::memory_order_relaxed);

            // Don't set on_data — let the write queue fill up
            platform_sleep_ms(2000);
        });

    client.connect(socketpp::inet4_address::loopback(port));
    client.start();

    wait_for(client_connected);

    if (client_connected.load())
    {
        // Try to overflow the write queue
        std::vector<char> big_chunk(max_write + 1, 'X');
        bool first_send = false;
        bool overflow_rejected = false;

        {
            std::lock_guard<std::mutex> lock(client_conn_mutex);

            if (client_conn_ptr)
            {
                first_send = client_conn_ptr->send(big_chunk.data(), max_write);
                platform_sleep_ms(10);
                overflow_rejected = !client_conn_ptr->send(big_chunk.data(), max_write);
            }
        }

        CHECK_MSG(first_send, "first send should succeed");
        CHECK_MSG(overflow_rejected, "overflow send should be rejected");
    }

    client.stop();
    server.stop();
}

// ===========================================================================
// TCP graceful close
// ===========================================================================

void test_tcp_graceful_close()
{
    const uint16_t port = 19879;

    socketpp::tcp4_server server;
    std::atomic<bool> close_fired {false};

    server.on_connect(
        [&close_fired](socketpp::tcp4_connection &conn)
        {
            conn.on_close([&close_fired]() { close_fired.store(true, std::memory_order_relaxed); });

            conn.on_data([](const char *, size_t) {});
        });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    {
        socketpp::tcp4_client client;
        std::atomic<bool> connected {false};

        client.on_connect(
            [&connected](socketpp::tcp4_connection &conn)
            {
                connected.store(true, std::memory_order_relaxed);
                conn.send("hello");
            });

        client.connect(socketpp::inet4_address::loopback(port));
        client.start();

        wait_for(connected);
        platform_sleep_ms(50);

        client.stop();
    }

    auto ok = wait_for(close_fired);
    CHECK_MSG(ok, "on_close not fired after client disconnect");

    server.stop();
}

// ===========================================================================
// TCP server stop with active connections
// ===========================================================================

void test_tcp_server_stop_with_connections()
{
    const uint16_t port = 19880;

    socketpp::tcp4_server server;
    std::atomic<int> connect_count {0};

    server.on_connect(
        [&connect_count](socketpp::tcp4_connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
        });

    auto r = server.listen(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    // Connect 3 clients
    std::vector<std::unique_ptr<socketpp::tcp4_client>> clients;

    for (int i = 0; i < 3; ++i)
    {
        auto client = std::make_unique<socketpp::tcp4_client>();
        client->on_connect([](socketpp::tcp4_connection &conn) { conn.send("keep-alive"); });
        client->connect(socketpp::inet4_address::loopback(port));
        client->start();
        clients.push_back(std::move(client));
    }

    wait_for_count(connect_count, 3);
    CHECK(connect_count.load() >= 3);

    // Stop server while connections are active — should not hang or crash
    auto start = std::chrono::steady_clock::now();
    server.stop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(10));

    for (auto &c : clients)
        c->stop();
}

// ===========================================================================
// UDP echo round trip
// ===========================================================================

void test_udp_echo_round_trip()
{
    const uint16_t port = 19881;

    socketpp::udp4_server server;

    server.on_message([&server](const char *data, size_t len, const socketpp::inet4_address &from)
                      { server.send_to(data, len, from); });

    auto r = server.bind(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    // Send a datagram using low-level UDP socket
    socketpp::udp4_socket sender;
    auto open_r = sender.open(socketpp::inet4_address::loopback(19920));
    CHECK_MSG(open_r, "sender open failed");

    const std::string msg = "Hello UDP high-level!";
    auto send_r = sender.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));
    CHECK_MSG(send_r, "send_to failed");

    // Wait for echo to arrive (server processes on thread pool)
    sender.set_non_blocking(true);
    std::array<char, 1500> buf {};
    socketpp::inet4_address from;
    socketpp::result<size_t> recv_r = socketpp::make_error_code(socketpp::errc::would_block);
    for (int i = 0; i < 100; ++i)
    {
        recv_r = sender.recv_from(buf.data(), buf.size(), from);
        if (recv_r)
            break;
        platform_sleep_ms(10);
    }
    CHECK_MSG(recv_r, "recv_from failed");

    if (recv_r)
    {
        CHECK(recv_r.value() == msg.size());
        CHECK(std::string(buf.data(), recv_r.value()) == msg);
    }

    sender.close();
    server.stop();
}

// ===========================================================================
// UDP concurrent messages
// ===========================================================================

void test_udp_concurrent_messages()
{
    const uint16_t port = 19882;
    const int num_messages = 20;

    socketpp::udp4_server server;
    std::atomic<int> recv_count {0};

    server.on_message(
        [&server, &recv_count](const char *data, size_t len, const socketpp::inet4_address &from)
        {
            recv_count.fetch_add(1, std::memory_order_relaxed);
            server.send_to(data, len, from);
        });

    auto r = server.bind(socketpp::inet4_address::loopback(port));
    if (!r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    server.start();
    platform_sleep_ms(50);

    socketpp::udp4_socket sender;
    sender.open(socketpp::inet4_address::loopback(19921));
    sender.set_non_blocking(true);

    for (int i = 0; i < num_messages; ++i)
    {
        std::string msg = "msg-" + std::to_string(i);
        sender.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));
    }

    // Wait for server to process all messages
    for (int i = 0; i < 200 && recv_count.load() < num_messages; ++i)
        platform_sleep_ms(10);

    // Receive echoes
    std::atomic<int> echo_count {0};

    for (int i = 0; i < num_messages; ++i)
    {
        std::array<char, 1500> buf {};
        socketpp::inet4_address from;

        socketpp::result<size_t> recv_r = socketpp::make_error_code(socketpp::errc::would_block);
        for (int j = 0; j < 50; ++j)
        {
            recv_r = sender.recv_from(buf.data(), buf.size(), from);
            if (recv_r)
                break;
            platform_sleep_ms(5);
        }

        if (recv_r)
            echo_count.fetch_add(1, std::memory_order_relaxed);
    }

    CHECK(recv_count.load() >= num_messages);
    CHECK(echo_count.load() >= num_messages);

    sender.close();
    server.stop();
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "high-level API tests\n";

    std::cerr << "\n--- thread pool ---\n";
    RUN_TEST(test_thread_pool_submit_and_execute);

    std::cerr << "\n--- tcp echo ---\n";
    RUN_TEST(test_tcp_echo_round_trip);

    std::cerr << "\n--- tcp multiple clients ---\n";
    RUN_TEST(test_tcp_multiple_clients);

    std::cerr << "\n--- tcp back-pressure ---\n";
    RUN_TEST(test_tcp_back_pressure);

    std::cerr << "\n--- tcp graceful close ---\n";
    RUN_TEST(test_tcp_graceful_close);

    std::cerr << "\n--- tcp server stop ---\n";
    RUN_TEST(test_tcp_server_stop_with_connections);

    std::cerr << "\n--- udp echo ---\n";
    RUN_TEST(test_udp_echo_round_trip);

    std::cerr << "\n--- udp concurrent ---\n";
    RUN_TEST(test_udp_concurrent_messages);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

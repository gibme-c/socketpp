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
/// Tests for the high-level stream and dgram API.

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
// Thread pool (verified via stream4 — callbacks must run on pool threads)
// ===========================================================================

void test_thread_pool_submit_and_execute()
{
    const uint16_t port = 19874;
    std::atomic<bool> callback_ran {false};
    auto main_tid = std::this_thread::get_id();
    bool ran_on_different_thread = false;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    server.on_connect(
        [&](socketpp::stream4::connection &conn)
        {
            ran_on_different_thread = (std::this_thread::get_id() != main_tid);
            callback_ran.store(true);
            conn.on_data([](const char *, size_t) {});
        });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    client.on_connect([](socketpp::stream4::connection &) {});

    wait_for(callback_ran);
    CHECK(callback_ran.load());
    CHECK(ran_on_different_thread);
}

// ===========================================================================
// stream4 echo round trip
// ===========================================================================

void test_stream_echo_round_trip()
{
    const uint16_t port = 19876;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<bool> echo_received {false};
    std::string echo_data;
    std::mutex echo_mutex;

    server.on_connect([](socketpp::stream4::connection &conn)
                      { conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); }); });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    client.on_connect(
        [&echo_received, &echo_data, &echo_mutex](socketpp::stream4::connection &conn)
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

    auto ok = wait_for(echo_received);
    CHECK_MSG(ok, "echo response not received within timeout");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(echo_mutex);
        CHECK(echo_data == "Hello, high-level API!");
    }
}

// ===========================================================================
// stream4 multiple clients
// ===========================================================================

void test_stream_multiple_clients()
{
    const uint16_t port = 19877;
    const int num_clients = 5;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> connect_count {0};

    server.on_connect(
        [&connect_count](socketpp::stream4::connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
        });

    platform_sleep_ms(50);

    std::atomic<int> echo_count {0};
    std::vector<socketpp::stream4> clients;

    for (int i = 0; i < num_clients; ++i)
    {
        auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
        CHECK_MSG(client_r, "client connect setup failed");
        if (!client_r)
            return;

        auto client = std::move(client_r.value());

        client.on_connect(
            [&echo_count, i](socketpp::stream4::connection &conn)
            {
                conn.on_data([&echo_count](const char *, size_t)
                             { echo_count.fetch_add(1, std::memory_order_relaxed); });

                std::string msg = "client-" + std::to_string(i);
                conn.send(msg);
            });

        clients.push_back(std::move(client));
        platform_sleep_ms(20);
    }

    auto ok = wait_for_count(echo_count, num_clients, 10000);
    CHECK_MSG(ok, "not all echoes received");
    CHECK(connect_count.load() >= num_clients);
}

// ===========================================================================
// stream4 back-pressure (write queue full)
// ===========================================================================

void test_stream_back_pressure()
{
    const uint16_t port = 19878;
    const size_t max_write = 4096;

    socketpp::stream_listen_config listen_cfg;
    listen_cfg.max_write_buffer = max_write;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port), listen_cfg);
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<bool> connected {false};
    socketpp::stream4::connection *server_conn_ptr = nullptr;
    std::mutex conn_mutex;

    server.on_connect(
        [&connected, &server_conn_ptr, &conn_mutex](socketpp::stream4::connection &conn)
        {
            {
                std::lock_guard<std::mutex> lock(conn_mutex);
                server_conn_ptr = &conn;
            }
            connected.store(true, std::memory_order_relaxed);

            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });

            // Keep the callback alive long enough
            platform_sleep_ms(2000);
        });

    platform_sleep_ms(50);

    socketpp::stream_connect_config connect_cfg;
    connect_cfg.max_write_buffer = max_write;

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port), connect_cfg);
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<bool> client_connected {false};
    socketpp::stream4::connection *client_conn_ptr = nullptr;
    std::mutex client_conn_mutex;

    client.on_connect(
        [&client_connected, &client_conn_ptr, &client_conn_mutex](socketpp::stream4::connection &conn)
        {
            {
                std::lock_guard<std::mutex> lock(client_conn_mutex);
                client_conn_ptr = &conn;
            }
            client_connected.store(true, std::memory_order_relaxed);

            // Don't set on_data — let the write queue fill up
            platform_sleep_ms(2000);
        });

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
}

// ===========================================================================
// stream4 graceful close
// ===========================================================================

void test_stream_graceful_close()
{
    const uint16_t port = 19879;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<bool> close_fired {false};

    server.on_connect(
        [&close_fired](socketpp::stream4::connection &conn)
        {
            conn.on_close([&close_fired]() { close_fired.store(true, std::memory_order_relaxed); });

            conn.on_data([](const char *, size_t) {});
        });

    platform_sleep_ms(50);

    {
        auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
        CHECK_MSG(client_r, "client connect setup failed");
        if (!client_r)
            return;

        auto client = std::move(client_r.value());
        std::atomic<bool> connected {false};

        client.on_connect(
            [&connected](socketpp::stream4::connection &conn)
            {
                connected.store(true, std::memory_order_relaxed);
                conn.send("hello");
            });

        wait_for(connected);
        platform_sleep_ms(50);

        // client destructor closes the connection
    }

    auto ok = wait_for(close_fired);
    CHECK_MSG(ok, "on_close not fired after client disconnect");
}

// ===========================================================================
// stream4 server stop with active connections
// ===========================================================================

void test_stream_stop_with_connections()
{
    const uint16_t port = 19880;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> connect_count {0};

    server.on_connect(
        [&connect_count](socketpp::stream4::connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
        });

    platform_sleep_ms(50);

    // Connect 3 clients
    std::vector<socketpp::stream4> clients;

    for (int i = 0; i < 3; ++i)
    {
        auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
        CHECK_MSG(client_r, "client connect setup failed");
        if (!client_r)
            return;

        auto client = std::move(client_r.value());
        client.on_connect([](socketpp::stream4::connection &conn) { conn.send("keep-alive"); });
        clients.push_back(std::move(client));
    }

    wait_for_count(connect_count, 3);
    CHECK(connect_count.load() >= 3);

    // Destroy server while connections are active — should not hang or crash
    auto start = std::chrono::steady_clock::now();
    {
        auto temp = std::move(server); // destructor of temp will clean up
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(10));
}

// ===========================================================================
// stream4 connection pause/resume
// ===========================================================================

void test_stream_connection_pause_resume()
{
    const uint16_t port = 19884;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    // Echo pattern: server echoes data. Client counts echoes to verify flow.
    // Capture pause/resume/send operations as lambdas to call from main thread.
    std::atomic<int> echo_count {0};
    std::atomic<bool> server_connected {false};
    std::atomic<bool> client_connected {false};

    std::function<void()> do_pause;
    std::function<void()> do_resume;
    std::function<bool()> is_paused;
    std::function<bool(const std::string &)> do_send;

    server.on_connect(
        [&](socketpp::stream4::connection &conn)
        {
            do_pause = [&conn]() { conn.pause(); };
            do_resume = [&conn]() { conn.resume(); };
            is_paused = [&conn]() { return conn.paused(); };

            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
            server_connected.store(true, std::memory_order_relaxed);
        });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    client.on_connect(
        [&](socketpp::stream4::connection &conn)
        {
            do_send = [&conn](const std::string &msg) { return conn.send(msg); };

            conn.on_data([&echo_count](const char *, size_t) { echo_count.fetch_add(1, std::memory_order_relaxed); });
            client_connected.store(true, std::memory_order_relaxed);
        });

    wait_for(server_connected);
    wait_for(client_connected);

    if (!server_connected.load() || !client_connected.load())
        return;

    // Allow start_reading() to complete (posted after on_connect returns)
    platform_sleep_ms(200);

    // Send a message — should be echoed back
    do_send("msg1");

    for (int i = 0; i < 100 && echo_count.load() < 1; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(echo_count.load() >= 1, "should receive echo before pause");

    // Pause the server-side connection
    do_pause();
    CHECK(is_paused());
    platform_sleep_ms(100);

    // Resume and verify
    do_resume();
    CHECK(!is_paused());
    platform_sleep_ms(100);

    int count_before = echo_count.load();
    do_send("msg2");

    for (int i = 0; i < 100 && echo_count.load() <= count_before; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(echo_count.load() > count_before, "should receive echo after resume");
}

// ===========================================================================
// dgram4 echo round trip
// ===========================================================================

void test_dgram_echo_round_trip()
{
    const uint16_t port = 19881;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    server.on_data([&server](const char *data, size_t len, const socketpp::inet4_address &from)
                   { server.send_to(data, len, from); });

    // Use a second dgram4 as the client
    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<bool> echo_received {false};
    std::string echo_data;
    std::mutex echo_mutex;

    client.on_data(
        [&](const char *data, size_t len, const socketpp::inet4_address &)
        {
            std::lock_guard<std::mutex> lock(echo_mutex);
            echo_data.assign(data, len);
            echo_received.store(true, std::memory_order_relaxed);
        });

    platform_sleep_ms(50);

    const std::string msg = "Hello dgram4!";
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));

    auto ok = wait_for(echo_received);
    CHECK_MSG(ok, "echo not received within timeout");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(echo_mutex);
        CHECK(echo_data == msg);
    }
}

// ===========================================================================
// dgram4 concurrent messages
// ===========================================================================

void test_dgram_concurrent_messages()
{
    const uint16_t port = 19882;
    const int num_messages = 20;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> recv_count {0};

    server.on_data(
        [&server, &recv_count](const char *data, size_t len, const socketpp::inet4_address &from)
        {
            recv_count.fetch_add(1, std::memory_order_relaxed);
            server.send_to(data, len, from);
        });

    // Use a second dgram4 as the client
    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<int> echo_count {0};

    client.on_data([&echo_count](const char *, size_t, const socketpp::inet4_address &)
                   { echo_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(50);

    for (int i = 0; i < num_messages; ++i)
    {
        std::string msg = "msg-" + std::to_string(i);
        client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));
    }

    // Wait for all messages to be received and echoed
    auto ok_recv = wait_for_count(recv_count, num_messages, 10000);
    auto ok_echo = wait_for_count(echo_count, num_messages, 10000);

    CHECK_MSG(ok_recv, "not all messages received by server");
    CHECK_MSG(ok_echo, "not all echoes received by client");
}

// ===========================================================================
// dgram4 ephemeral port
// ===========================================================================

void test_dgram_ephemeral_port()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "create with port 0 should succeed");

    if (r)
    {
        auto &d = r.value();
        auto addr = d.local_addr();
        CHECK_MSG(addr.port() != 0, "ephemeral port should be non-zero");
    }
}

// ===========================================================================
// dgram4 pause/resume
// ===========================================================================

void test_dgram_pause_resume()
{
    const uint16_t port = 19883;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> recv_count {0};

    server.on_data([&recv_count](const char *, size_t, const socketpp::inet4_address &)
                   { recv_count.fetch_add(1, std::memory_order_relaxed); });

    // Use a second dgram4 as the sender
    auto sender_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(sender_r, "sender create failed");
    if (!sender_r)
        return;

    auto sender = std::move(sender_r.value());

    platform_sleep_ms(50);

    // Send a message, should be received
    const std::string msg = "hello";
    sender.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));

    for (int i = 0; i < 100 && recv_count.load() < 1; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(recv_count.load() >= 1, "should receive message before pause");

    // Pause and verify state
    server.pause();
    CHECK(server.paused());
    platform_sleep_ms(50);

    int count_at_pause = recv_count.load();

    // Send during pause -- may or may not be received depending on kernel buffer
    // but the library should not crash or hang
    sender.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));
    platform_sleep_ms(100);

    // Resume and send another message -- this one should definitely be received
    server.resume();
    CHECK(!server.paused());
    platform_sleep_ms(50);

    sender.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));

    for (int i = 0; i < 100 && recv_count.load() <= count_at_pause; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(recv_count.load() > count_at_pause, "should receive message after resume");
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "high-level API tests\n";

    std::cerr << "\n--- thread pool ---\n";
    RUN_TEST(test_thread_pool_submit_and_execute);

    std::cerr << "\n--- stream echo ---\n";
    RUN_TEST(test_stream_echo_round_trip);

    std::cerr << "\n--- stream multiple clients ---\n";
    RUN_TEST(test_stream_multiple_clients);

    std::cerr << "\n--- stream back-pressure ---\n";
    RUN_TEST(test_stream_back_pressure);

    std::cerr << "\n--- stream graceful close ---\n";
    RUN_TEST(test_stream_graceful_close);

    std::cerr << "\n--- stream server stop ---\n";
    RUN_TEST(test_stream_stop_with_connections);

    std::cerr << "\n--- stream connection pause/resume ---\n";
    RUN_TEST(test_stream_connection_pause_resume);

    std::cerr << "\n--- dgram echo ---\n";
    RUN_TEST(test_dgram_echo_round_trip);

    std::cerr << "\n--- dgram concurrent ---\n";
    RUN_TEST(test_dgram_concurrent_messages);

    std::cerr << "\n--- dgram ephemeral port ---\n";
    RUN_TEST(test_dgram_ephemeral_port);

    std::cerr << "\n--- dgram pause/resume ---\n";
    RUN_TEST(test_dgram_pause_resume);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

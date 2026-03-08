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
// dgram4 batch send
// ===========================================================================

void test_dgram4_batch_send()
{
    const uint16_t port = 19885;
    const int num_messages = 10;

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

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    platform_sleep_ms(50);

    // Build batch
    std::vector<std::string> payloads(num_messages);
    std::vector<socketpp::dgram4_send_entry> entries(num_messages);

    for (int i = 0; i < num_messages; ++i)
    {
        payloads[i] = "batch-" + std::to_string(i);
        entries[i].data = payloads[i].data();
        entries[i].len = payloads[i].size();
        entries[i].dest = socketpp::inet4_address::loopback(port);
    }

    auto send_r = client.send_batch(socketpp::span<const socketpp::dgram4_send_entry>(entries));
    CHECK_MSG(send_r, "send_batch should succeed");

    if (send_r)
        CHECK_MSG(send_r.value() == num_messages, "send_batch should send all messages");

    auto ok = wait_for_count(recv_count, num_messages, 5000);
    CHECK_MSG(ok, "server should receive all batch-sent messages");
}

// ===========================================================================
// dgram4 batch recv
// ===========================================================================

void test_dgram4_batch_recv()
{
    const uint16_t port = 19886;
    const int num_messages = 10;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> recv_count {0};
    std::atomic<int> batch_calls {0};

    server.on_data_batch(
        [&recv_count, &batch_calls](socketpp::span<const socketpp::dgram4_message> msgs)
        {
            batch_calls.fetch_add(1, std::memory_order_relaxed);
            recv_count.fetch_add(static_cast<int>(msgs.size()), std::memory_order_relaxed);
        });

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    platform_sleep_ms(50);

    for (int i = 0; i < num_messages; ++i)
    {
        std::string msg = "msg-" + std::to_string(i);
        client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(port));
    }

    auto ok = wait_for_count(recv_count, num_messages, 5000);
    CHECK_MSG(ok, "batch recv should receive all messages");
    CHECK_MSG(batch_calls.load() >= 1, "batch callback should be called at least once");
}

// ===========================================================================
// dgram4 batch roundtrip
// ===========================================================================

void test_dgram4_batch_roundtrip()
{
    const uint16_t port = 19887;
    const int num_messages = 8;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    // Server echoes back each batch via send_batch
    server.on_data_batch(
        [&server](socketpp::span<const socketpp::dgram4_message> msgs)
        {
            std::vector<socketpp::dgram4_send_entry> replies(msgs.size());

            for (size_t i = 0; i < msgs.size(); ++i)
            {
                replies[i].data = msgs[i].data;
                replies[i].len = msgs[i].len;
                replies[i].dest = msgs[i].from;
            }

            server.send_batch(socketpp::span<const socketpp::dgram4_send_entry>(replies));
        });

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<int> echo_count {0};

    client.on_data([&echo_count](const char *, size_t, const socketpp::inet4_address &)
                   { echo_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(50);

    // Send batch from client
    std::vector<std::string> payloads(num_messages);
    std::vector<socketpp::dgram4_send_entry> entries(num_messages);

    for (int i = 0; i < num_messages; ++i)
    {
        payloads[i] = "rt-" + std::to_string(i);
        entries[i].data = payloads[i].data();
        entries[i].len = payloads[i].size();
        entries[i].dest = socketpp::inet4_address::loopback(port);
    }

    auto send_r = client.send_batch(socketpp::span<const socketpp::dgram4_send_entry>(entries));
    CHECK_MSG(send_r, "send_batch should succeed");

    auto ok = wait_for_count(echo_count, num_messages, 5000);
    CHECK_MSG(ok, "client should receive all echoed messages");
}

// ===========================================================================
// dgram4 defer (one-shot timer)
// ===========================================================================

void test_dgram4_defer()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "create failed");
    if (!r)
        return;

    auto &d = r.value();
    std::atomic<bool> fired {false};
    auto start = std::chrono::steady_clock::now();

    auto h = d.defer(std::chrono::milliseconds(50), [&fired]() { fired.store(true, std::memory_order_relaxed); });

    CHECK_MSG(h, "defer should return valid handle");
    auto ok = wait_for(fired, 3000);
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK_MSG(ok, "defer callback should fire");
    CHECK_MSG(elapsed >= std::chrono::milliseconds(40), "should not fire too early");
    CHECK_MSG(elapsed < std::chrono::milliseconds(2000), "should not fire too late");
}

// ===========================================================================
// dgram4 repeat (repeating timer)
// ===========================================================================

void test_dgram4_repeat()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "create failed");
    if (!r)
        return;

    auto &d = r.value();
    std::atomic<int> count {0};

    auto h = d.repeat(std::chrono::milliseconds(30), [&count]() { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK_MSG(h, "repeat should return valid handle");

    auto ok = wait_for_count(count, 3, 3000);
    CHECK_MSG(ok, "repeat callback should fire at least 3 times");

    h.cancel();
    platform_sleep_ms(100);

    int after_cancel = count.load(std::memory_order_relaxed);
    platform_sleep_ms(200);

    int after_wait = count.load(std::memory_order_relaxed);
    CHECK_MSG(after_wait - after_cancel <= 1, "timer should stop after cancel");
}

// ===========================================================================
// dgram4 defer cancel (cancelled one-shot never fires)
// ===========================================================================

void test_dgram4_defer_cancel()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "create failed");
    if (!r)
        return;

    auto &d = r.value();
    std::atomic<bool> fired {false};

    auto h = d.defer(std::chrono::milliseconds(200), [&fired]() { fired.store(true, std::memory_order_relaxed); });

    h.cancel();
    platform_sleep_ms(500);

    CHECK_MSG(!fired.load(), "cancelled defer should not fire");
}

// ===========================================================================
// dgram4 post
// ===========================================================================

void test_dgram4_post()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "create failed");
    if (!r)
        return;

    auto &d = r.value();
    std::atomic<bool> fired {false};
    auto main_tid = std::this_thread::get_id();
    std::atomic<bool> different_thread {false};

    d.post(
        [&fired, &different_thread, main_tid]()
        {
            different_thread.store(std::this_thread::get_id() != main_tid, std::memory_order_relaxed);
            fired.store(true, std::memory_order_relaxed);
        });

    auto ok = wait_for(fired, 3000);
    CHECK_MSG(ok, "post callback should fire");
    CHECK_MSG(different_thread.load(), "post callback should run on thread pool, not main thread");
}

// ===========================================================================
// stream4 defer (one-shot timer)
// ===========================================================================

void test_stream4_defer()
{
    auto r = socketpp::stream4::listen(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "listen failed");
    if (!r)
        return;

    auto &s = r.value();
    std::atomic<bool> fired {false};
    auto start = std::chrono::steady_clock::now();

    auto h = s.defer(std::chrono::milliseconds(50), [&fired]() { fired.store(true, std::memory_order_relaxed); });

    CHECK_MSG(h, "defer should return valid handle");
    auto ok = wait_for(fired, 3000);
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK_MSG(ok, "defer callback should fire");
    CHECK_MSG(elapsed >= std::chrono::milliseconds(40), "should not fire too early");
    CHECK_MSG(elapsed < std::chrono::milliseconds(2000), "should not fire too late");
}

// ===========================================================================
// stream4 repeat (repeating timer)
// ===========================================================================

void test_stream4_repeat()
{
    auto r = socketpp::stream4::listen(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "listen failed");
    if (!r)
        return;

    auto &s = r.value();
    std::atomic<int> count {0};

    auto h = s.repeat(std::chrono::milliseconds(30), [&count]() { count.fetch_add(1, std::memory_order_relaxed); });

    CHECK_MSG(h, "repeat should return valid handle");

    auto ok = wait_for_count(count, 3, 3000);
    CHECK_MSG(ok, "repeat callback should fire at least 3 times");

    h.cancel();
    platform_sleep_ms(100);

    int after_cancel = count.load(std::memory_order_relaxed);
    platform_sleep_ms(200);

    int after_wait = count.load(std::memory_order_relaxed);
    CHECK_MSG(after_wait - after_cancel <= 1, "timer should stop after cancel");
}

// ===========================================================================
// stream4 defer cancel (cancelled one-shot never fires)
// ===========================================================================

void test_stream4_defer_cancel()
{
    auto r = socketpp::stream4::listen(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "listen failed");
    if (!r)
        return;

    auto &s = r.value();
    std::atomic<bool> fired {false};

    auto h = s.defer(std::chrono::milliseconds(200), [&fired]() { fired.store(true, std::memory_order_relaxed); });

    h.cancel();
    platform_sleep_ms(500);

    CHECK_MSG(!fired.load(), "cancelled defer should not fire");
}

// ===========================================================================
// stream4 post
// ===========================================================================

void test_stream4_post()
{
    auto r = socketpp::stream4::listen(socketpp::inet4_address::loopback(0));
    CHECK_MSG(r, "listen failed");
    if (!r)
        return;

    auto &s = r.value();
    std::atomic<bool> fired {false};
    auto main_tid = std::this_thread::get_id();
    std::atomic<bool> different_thread {false};

    s.post(
        [&fired, &different_thread, main_tid]()
        {
            different_thread.store(std::this_thread::get_id() != main_tid, std::memory_order_relaxed);
            fired.store(true, std::memory_order_relaxed);
        });

    auto ok = wait_for(fired, 3000);
    CHECK_MSG(ok, "post callback should fire");
    CHECK_MSG(different_thread.load(), "post callback should run on thread pool, not main thread");
}

// ===========================================================================
// dgram4 claim — peer receives, main on_data does not
// ===========================================================================

void test_dgram4_claim_basic()
{
    const uint16_t server_port = 19888;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    // Claim the client's address on the server side
    auto peer_r = server.claim(client_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());
    CHECK(peer.is_open());
    CHECK(peer.peer_addr().port() == client_addr.port());

    std::atomic<int> peer_recv_count {0};
    std::atomic<int> main_recv_count {0};

    peer.on_data([&peer_recv_count](const char *, size_t) { peer_recv_count.fetch_add(1, std::memory_order_relaxed); });

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet4_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(100);

    // Send from the claimed client address to server
    const std::string msg = "from-claimed";
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(server_port));

    auto ok = wait_for_count(peer_recv_count, 1, 5000);
    CHECK_MSG(ok, "peer handle should receive datagram from claimed address");

    platform_sleep_ms(100);
    CHECK_MSG(main_recv_count.load() == 0, "main on_data should NOT fire for claimed peer");
}

// ===========================================================================
// dgram4 claim — double claim returns error
// ===========================================================================

void test_dgram4_double_claim()
{
    const uint16_t port = 19889;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    auto peer_addr = socketpp::inet4_address::loopback(12345);

    auto p1 = server.claim(peer_addr);
    CHECK_MSG(p1, "first claim should succeed");

    auto p2 = server.claim(peer_addr);
    CHECK_MSG(!p2, "second claim of same address should fail");
}

// ===========================================================================
// dgram4 relinquish — traffic returns to main on_data
// ===========================================================================

void test_dgram4_relinquish()
{
    const uint16_t server_port = 19890;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<int> peer_recv_count {0};
    std::atomic<int> main_recv_count {0};

    // Claim, receive on peer, then relinquish
    auto peer_r = server.claim(client_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());
    peer.on_data([&peer_recv_count](const char *, size_t) { peer_recv_count.fetch_add(1, std::memory_order_relaxed); });

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet4_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(100);

    // Send while claimed — peer should get it
    const std::string msg = "before-relinquish";
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(server_port));

    wait_for_count(peer_recv_count, 1, 5000);
    CHECK_MSG(peer_recv_count.load() >= 1, "peer should receive before relinquish");

    // Relinquish
    peer.relinquish();
    CHECK(!peer.is_open());
    platform_sleep_ms(200);

    // Send again — should go to main on_data
    const std::string msg2 = "after-relinquish";
    client.send_to(msg2.data(), msg2.size(), socketpp::inet4_address::loopback(server_port));

    auto ok = wait_for_count(main_recv_count, 1, 5000);
    CHECK_MSG(ok, "main on_data should receive after relinquish");
}

// ===========================================================================
// dgram4 destructor auto-relinquish
// ===========================================================================

void test_dgram4_destructor_relinquish()
{
    const uint16_t server_port = 19891;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<int> main_recv_count {0};

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet4_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    // Claim then let peer go out of scope
    {
        auto peer_r = server.claim(client_addr);
        CHECK_MSG(peer_r, "claim should succeed");
        if (!peer_r)
            return;

        auto peer = std::move(peer_r.value());
        peer.on_data([](const char *, size_t) {});
        platform_sleep_ms(100);
        // peer destructor calls relinquish()
    }

    platform_sleep_ms(200);

    // Send — should go to main on_data since peer was destroyed
    const std::string msg = "after-destroy";
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(server_port));

    auto ok = wait_for_count(main_recv_count, 1, 5000);
    CHECK_MSG(ok, "main on_data should receive after peer destructor");
}

// ===========================================================================
// dgram4 peer send
// ===========================================================================

void test_dgram4_peer_send()
{
    const uint16_t server_port = 19892;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<bool> client_recv {false};
    std::string received_data;
    std::mutex recv_mutex;

    client.on_data(
        [&](const char *data, size_t len, const socketpp::inet4_address &)
        {
            std::lock_guard<std::mutex> lock(recv_mutex);
            received_data.assign(data, len);
            client_recv.store(true, std::memory_order_relaxed);
        });

    // Claim and send through peer handle
    auto peer_r = server.claim(client_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());
    peer.on_data([](const char *, size_t) {});
    platform_sleep_ms(100);

    const std::string msg = "from-peer-handle";
    bool sent = peer.send(msg.data(), msg.size());
    CHECK_MSG(sent, "peer send should succeed");

    auto ok = wait_for(client_recv, 5000);
    CHECK_MSG(ok, "client should receive datagram from peer handle");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(recv_mutex);
        CHECK(received_data == msg);
    }
}

// ===========================================================================
// dgram4 peer timer/post
// ===========================================================================

void test_dgram4_peer_timer_post()
{
    const uint16_t port = 19893;

    auto server_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    auto peer_addr = socketpp::inet4_address::loopback(55555);

    auto peer_r = server.claim(peer_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());

    // Test defer on peer
    std::atomic<bool> defer_fired {false};
    auto h = peer.defer(
        std::chrono::milliseconds(50), [&defer_fired]() { defer_fired.store(true, std::memory_order_relaxed); });
    CHECK_MSG(h, "peer defer should return valid handle");

    auto ok = wait_for(defer_fired, 3000);
    CHECK_MSG(ok, "peer defer callback should fire");

    // Test post on peer
    std::atomic<bool> post_fired {false};
    peer.post([&post_fired]() { post_fired.store(true, std::memory_order_relaxed); });

    ok = wait_for(post_fired, 3000);
    CHECK_MSG(ok, "peer post callback should fire");
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

    std::cerr << "\n--- dgram batch send ---\n";
    RUN_TEST(test_dgram4_batch_send);

    std::cerr << "\n--- dgram batch recv ---\n";
    RUN_TEST(test_dgram4_batch_recv);

    std::cerr << "\n--- dgram batch roundtrip ---\n";
    RUN_TEST(test_dgram4_batch_roundtrip);

    std::cerr << "\n--- dgram4 defer ---\n";
    RUN_TEST(test_dgram4_defer);

    std::cerr << "\n--- dgram4 repeat ---\n";
    RUN_TEST(test_dgram4_repeat);

    std::cerr << "\n--- dgram4 defer cancel ---\n";
    RUN_TEST(test_dgram4_defer_cancel);

    std::cerr << "\n--- dgram4 post ---\n";
    RUN_TEST(test_dgram4_post);

    std::cerr << "\n--- stream4 defer ---\n";
    RUN_TEST(test_stream4_defer);

    std::cerr << "\n--- stream4 repeat ---\n";
    RUN_TEST(test_stream4_repeat);

    std::cerr << "\n--- stream4 defer cancel ---\n";
    RUN_TEST(test_stream4_defer_cancel);

    std::cerr << "\n--- stream4 post ---\n";
    RUN_TEST(test_stream4_post);

    std::cerr << "\n--- dgram4 claim basic ---\n";
    RUN_TEST(test_dgram4_claim_basic);

    std::cerr << "\n--- dgram4 double claim ---\n";
    RUN_TEST(test_dgram4_double_claim);

    std::cerr << "\n--- dgram4 relinquish ---\n";
    RUN_TEST(test_dgram4_relinquish);

    std::cerr << "\n--- dgram4 destructor relinquish ---\n";
    RUN_TEST(test_dgram4_destructor_relinquish);

    std::cerr << "\n--- dgram4 peer send ---\n";
    RUN_TEST(test_dgram4_peer_send);

    std::cerr << "\n--- dgram4 peer timer/post ---\n";
    RUN_TEST(test_dgram4_peer_timer_post);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

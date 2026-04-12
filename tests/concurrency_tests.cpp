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

/// @file concurrency_tests.cpp
/// Concurrency stress tests for socketpp.

#include "test_harness.hpp"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <socketpp.hpp>
#include <socketpp/event/loop.hpp>
#include <string>
#include <vector>

using socketpp_test::platform_sleep_ms;
using socketpp_test::wait_for;
using socketpp_test::wait_for_count;

// ===========================================================================
// stream4 concurrent send from multiple threads
// ===========================================================================

void test_stream4_concurrent_send()
{
    const uint16_t port = 39876;
    const int num_threads = 4;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> total_bytes_received {0};

    server.on_connect(
        [&total_bytes_received](socketpp::stream4::connection &conn)
        {
            conn.on_data([&total_bytes_received](const char *, size_t len)
                         { total_bytes_received.fetch_add(static_cast<int>(len), std::memory_order_relaxed); });
        });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<bool> client_connected {false};
    std::function<bool(const std::string &)> do_send;

    client.on_connect(
        [&client_connected, &do_send](socketpp::stream4::connection &conn)
        {
            do_send = [&conn](const std::string &msg) { return conn.send(msg); };
            client_connected.store(true, std::memory_order_relaxed);
        });

    wait_for(client_connected);
    if (!client_connected.load())
        return;

    platform_sleep_ms(100);

    const std::string msg = "X"; // 1 byte per message
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&do_send, &msg]()
            {
                for (int i = 0; i < 100; ++i)
                    do_send(msg);
            });
    }

    for (auto &th : threads)
        th.join();

    // Wait for all data to arrive (may arrive in larger chunks)
    const int expected = num_threads * 100;

    auto ok = wait_for_count(total_bytes_received, expected, 15000);
    CHECK_MSG(ok, "not all concurrent data received by server");
}

// ===========================================================================
// dgram4 concurrent send from multiple threads
// ===========================================================================

void test_dgram4_concurrent_send()
{
    const uint16_t port = 39877;
    const int num_threads = 4;

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

    auto sender_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));
    CHECK_MSG(sender_r, "sender create failed");
    if (!sender_r)
        return;

    auto &sender = sender_r.value();

    platform_sleep_ms(50);

    std::vector<std::thread> threads;
    auto dest = socketpp::inet4_address::loopback(port);
    const std::string msg = "concurrent-dgram";

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&sender, &dest, &msg]()
            {
                for (int i = 0; i < 100; ++i)
                    sender.send_to(msg.data(), msg.size(), dest);
            });
    }

    for (auto &th : threads)
        th.join();

    // UDP may drop packets under load, so just verify no crash and some received
    platform_sleep_ms(500);
    CHECK_MSG(recv_count.load() > 0, "at least some concurrent UDP messages should arrive");
}

// ===========================================================================
// dgram4 claim from on_data callback
// ===========================================================================

void test_dgram4_claim_from_callback()
{
    const uint16_t server_port = 39878;

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

    std::atomic<bool> claim_succeeded {false};
    std::atomic<bool> peer_received {false};

    // Hold the peer outside the callback so it outlives the lambda invocation
    std::mutex peer_mutex;
    std::unique_ptr<socketpp::dgram4_peer> held_peer;

    // Claim from inside on_data callback
    server.on_data(
        [&](const char *, size_t, const socketpp::inet4_address &from)
        {
            auto peer_r = server.claim(from);

            if (peer_r)
            {
                claim_succeeded.store(true, std::memory_order_relaxed);

                std::lock_guard<std::mutex> lock(peer_mutex);
                held_peer = std::make_unique<socketpp::dgram4_peer>(std::move(peer_r.value()));
                held_peer->on_data([&peer_received](const char *, size_t)
                                   { peer_received.store(true, std::memory_order_relaxed); });
            }
        });

    platform_sleep_ms(50);

    const std::string msg = "trigger-claim";
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(server_port));

    auto ok = wait_for(claim_succeeded, 5000);
    CHECK_MSG(ok, "claim from callback should succeed");

    // Send a second message — should be routed to the peer handle
    platform_sleep_ms(100);
    client.send_to(msg.data(), msg.size(), socketpp::inet4_address::loopback(server_port));

    auto peer_ok = wait_for(peer_received, 5000);
    CHECK_MSG(peer_ok, "claimed peer should receive subsequent datagram");
}

// ===========================================================================
// stream4 rapid connect/disconnect
// ===========================================================================

void test_stream4_rapid_connect_disconnect()
{
    const uint16_t port = 39879;
    const int num_cycles = 20;

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> connect_count {0};
    std::atomic<int> close_count {0};

    server.on_connect(
        [&connect_count, &close_count](socketpp::stream4::connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);

            conn.on_close([&close_count]() { close_count.fetch_add(1, std::memory_order_relaxed); });

            conn.on_data([](const char *, size_t) {});
        });

    platform_sleep_ms(50);

    for (int i = 0; i < num_cycles; ++i)
    {
        auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));
        if (!client_r)
            continue;

        auto client = std::move(client_r.value());
        std::atomic<bool> connected {false};

        client.on_connect(
            [&connected](socketpp::stream4::connection &conn)
            {
                connected.store(true, std::memory_order_relaxed);
                conn.send("ping");
            });

        wait_for(connected, 2000);
        // client destructor closes
    }

    // Wait for close events to propagate
    wait_for_count(close_count, num_cycles, 15000);

    CHECK_MSG(connect_count.load() >= num_cycles, "all connections should have been accepted");
    CHECK_MSG(server.connection_count() == 0, "all connections should be closed after rapid cycle");
}

// ===========================================================================
// event_loop concurrent post
// ===========================================================================

void test_event_loop_concurrent_post()
{
    socketpp::event_loop loop;

    const int num_threads = 4;
    std::atomic<int> exec_count {0};

    std::thread runner([&loop]() { loop.run(); });

    platform_sleep_ms(50);

    std::vector<std::thread> posters;

    for (int t = 0; t < num_threads; ++t)
    {
        posters.emplace_back(
            [&loop, &exec_count]()
            {
                for (int i = 0; i < 100; ++i)
                    loop.post([&exec_count]() { exec_count.fetch_add(1, std::memory_order_relaxed); });
            });
    }

    for (auto &th : posters)
        th.join();

    const int expected = num_threads * 100;

    auto ok = wait_for_count(exec_count, expected, 10000);

    loop.stop();
    runner.join();

    CHECK_MSG(ok, "all concurrent posts should execute");
    CHECK(exec_count.load() == expected);
}

// ===========================================================================
// stream4 backpressure signal
// ===========================================================================

void test_stream4_backpressure_signal()
{
    const uint16_t port = 39880;
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

    server.on_connect(
        [](socketpp::stream4::connection &conn)
        {
            // Don't read -- let backpressure build
            conn.on_data([](const char *, size_t) {});
            platform_sleep_ms(3000);
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
    std::mutex conn_mutex;

    client.on_connect(
        [&client_connected, &client_conn_ptr, &conn_mutex](socketpp::stream4::connection &conn)
        {
            {
                std::lock_guard<std::mutex> lock(conn_mutex);
                client_conn_ptr = &conn;
            }
            client_connected.store(true, std::memory_order_relaxed);

            platform_sleep_ms(3000);
        });

    wait_for(client_connected);

    if (client_connected.load())
    {
        std::vector<char> chunk(max_write + 1, 'X');
        bool first_send = false;
        bool overflow_rejected = false;

        {
            std::lock_guard<std::mutex> lock(conn_mutex);

            if (client_conn_ptr)
            {
                first_send = client_conn_ptr->send(chunk.data(), max_write);
                platform_sleep_ms(10);
                overflow_rejected = !client_conn_ptr->send(chunk.data(), max_write);
            }
        }

        CHECK_MSG(first_send, "first send should succeed");
        CHECK_MSG(overflow_rejected, "overflow send should be rejected (backpressure)");
    }
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "concurrency tests\n";

    std::cerr << "\n--- stream4 concurrent send ---\n";
    RUN_TEST(test_stream4_concurrent_send);

    std::cerr << "\n--- dgram4 concurrent send ---\n";
    RUN_TEST(test_dgram4_concurrent_send);

    std::cerr << "\n--- dgram4 claim from callback ---\n";
    RUN_TEST(test_dgram4_claim_from_callback);

    std::cerr << "\n--- stream4 rapid connect/disconnect ---\n";
    RUN_TEST(test_stream4_rapid_connect_disconnect);

    std::cerr << "\n--- event_loop concurrent post ---\n";
    RUN_TEST(test_event_loop_concurrent_post);

    std::cerr << "\n--- stream4 backpressure signal ---\n";
    RUN_TEST(test_stream4_backpressure_signal);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

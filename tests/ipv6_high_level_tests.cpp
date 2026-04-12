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

/// @file ipv6_high_level_tests.cpp
/// Tests for the high-level stream6, dgram6, and dgram6_peer API.

#include "test_harness.hpp"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <socketpp.hpp>
#include <string>
#include <vector>

using socketpp_test::platform_sleep_ms;
using socketpp_test::wait_for;
using socketpp_test::wait_for_count;

// ===========================================================================
// stream6 echo round trip
// ===========================================================================

void test_stream6_echo_round_trip()
{
    const uint16_t port = 29876;

    auto server_r = socketpp::stream6::listen(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<bool> echo_received {false};
    std::string echo_data;
    std::mutex echo_mutex;

    server.on_connect([](socketpp::stream6::connection &conn)
                      { conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); }); });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream6::connect(socketpp::inet6_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    client.on_connect(
        [&echo_received, &echo_data, &echo_mutex](socketpp::stream6::connection &conn)
        {
            conn.on_data(
                [&echo_received, &echo_data, &echo_mutex](const char *data, size_t len)
                {
                    std::lock_guard<std::mutex> lock(echo_mutex);
                    echo_data.assign(data, len);
                    echo_received.store(true, std::memory_order_relaxed);
                });

            conn.send("Hello, IPv6 stream!");
        });

    auto ok = wait_for(echo_received);
    CHECK_MSG(ok, "echo response not received within timeout");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(echo_mutex);
        CHECK(echo_data == "Hello, IPv6 stream!");
    }
}

// ===========================================================================
// stream6 multiple clients
// ===========================================================================

void test_stream6_multi_client()
{
    const uint16_t port = 29877;
    const int num_clients = 3;

    auto server_r = socketpp::stream6::listen(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> connect_count {0};

    server.on_connect(
        [&connect_count](socketpp::stream6::connection &conn)
        {
            connect_count.fetch_add(1, std::memory_order_relaxed);
            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
        });

    platform_sleep_ms(50);

    std::atomic<int> echo_count {0};
    std::vector<socketpp::stream6> clients;

    for (int i = 0; i < num_clients; ++i)
    {
        auto client_r = socketpp::stream6::connect(socketpp::inet6_address::loopback(port));
        CHECK_MSG(client_r, "client connect setup failed");
        if (!client_r)
            return;

        auto client = std::move(client_r.value());

        client.on_connect(
            [&echo_count, i](socketpp::stream6::connection &conn)
            {
                conn.on_data([&echo_count](const char *, size_t)
                             { echo_count.fetch_add(1, std::memory_order_relaxed); });

                std::string msg = "v6-client-" + std::to_string(i);
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
// stream6 graceful close from server
// ===========================================================================

void test_stream6_close_from_server()
{
    const uint16_t port = 29878;

    auto server_r = socketpp::stream6::listen(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<bool> close_fired {false};

    server.on_connect(
        [&close_fired](socketpp::stream6::connection &conn)
        {
            conn.on_close([&close_fired]() { close_fired.store(true, std::memory_order_relaxed); });
            conn.on_data([](const char *, size_t) {});
        });

    platform_sleep_ms(50);

    {
        auto client_r = socketpp::stream6::connect(socketpp::inet6_address::loopback(port));
        CHECK_MSG(client_r, "client connect setup failed");
        if (!client_r)
            return;

        auto client = std::move(client_r.value());
        std::atomic<bool> connected {false};

        client.on_connect(
            [&connected](socketpp::stream6::connection &conn)
            {
                connected.store(true, std::memory_order_relaxed);
                conn.send("hello-v6");
            });

        wait_for(connected);
        platform_sleep_ms(50);
    }

    auto ok = wait_for(close_fired);
    CHECK_MSG(ok, "on_close not fired after client disconnect");
}

// ===========================================================================
// stream6 connection pause/resume
// ===========================================================================

void test_stream6_connection_pause_resume()
{
    const uint16_t port = 29879;

    auto server_r = socketpp::stream6::listen(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    std::atomic<int> echo_count {0};
    std::atomic<bool> server_connected {false};
    std::atomic<bool> client_connected {false};

    std::function<void()> do_pause;
    std::function<void()> do_resume;
    std::function<bool()> is_paused;
    std::function<bool(const std::string &)> do_send;

    server.on_connect(
        [&](socketpp::stream6::connection &conn)
        {
            do_pause = [&conn]() { conn.pause(); };
            do_resume = [&conn]() { conn.resume(); };
            is_paused = [&conn]() { return conn.paused(); };

            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });
            server_connected.store(true, std::memory_order_relaxed);
        });

    platform_sleep_ms(50);

    auto client_r = socketpp::stream6::connect(socketpp::inet6_address::loopback(port));
    CHECK_MSG(client_r, "client connect setup failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());

    client.on_connect(
        [&](socketpp::stream6::connection &conn)
        {
            do_send = [&conn](const std::string &msg) { return conn.send(msg); };
            conn.on_data([&echo_count](const char *, size_t) { echo_count.fetch_add(1, std::memory_order_relaxed); });
            client_connected.store(true, std::memory_order_relaxed);
        });

    wait_for(server_connected);
    wait_for(client_connected);

    if (!server_connected.load() || !client_connected.load())
        return;

    platform_sleep_ms(200);

    do_send("msg1");

    for (int i = 0; i < 100 && echo_count.load() < 1; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(echo_count.load() >= 1, "should receive echo before pause");

    do_pause();
    CHECK(is_paused());
    platform_sleep_ms(100);

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
// dgram6 echo round trip
// ===========================================================================

void test_dgram6_echo_round_trip()
{
    const uint16_t port = 29881;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    server.on_data([&server](const char *data, size_t len, const socketpp::inet6_address &from)
                   { server.send_to(data, len, from); });

    auto client_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    std::atomic<bool> echo_received {false};
    std::string echo_data;
    std::mutex echo_mutex;

    client.on_data(
        [&](const char *data, size_t len, const socketpp::inet6_address &)
        {
            std::lock_guard<std::mutex> lock(echo_mutex);
            echo_data.assign(data, len);
            echo_received.store(true, std::memory_order_relaxed);
        });

    platform_sleep_ms(50);

    const std::string msg = "Hello dgram6!";
    client.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(port));

    auto ok = wait_for(echo_received);
    CHECK_MSG(ok, "echo not received within timeout");

    if (ok)
    {
        std::lock_guard<std::mutex> lock(echo_mutex);
        CHECK(echo_data == msg);
    }
}

// ===========================================================================
// dgram6 ephemeral port
// ===========================================================================

void test_dgram6_ephemeral_port()
{
    auto r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    if (!r)
    {
        std::cerr << "(IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto &d = r.value();
    auto addr = d.local_addr();
    CHECK_MSG(addr.port() != 0, "ephemeral port should be non-zero");
}

// ===========================================================================
// dgram6 pause/resume
// ===========================================================================

void test_dgram6_pause_resume()
{
    const uint16_t port = 29882;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> recv_count {0};

    server.on_data([&recv_count](const char *, size_t, const socketpp::inet6_address &)
                   { recv_count.fetch_add(1, std::memory_order_relaxed); });

    auto sender_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(sender_r, "sender create failed");
    if (!sender_r)
        return;

    auto sender = std::move(sender_r.value());

    platform_sleep_ms(50);

    const std::string msg = "hello-v6";
    sender.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(port));

    for (int i = 0; i < 100 && recv_count.load() < 1; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(recv_count.load() >= 1, "should receive message before pause");

    server.pause();
    CHECK(server.paused());
    platform_sleep_ms(50);

    int count_at_pause = recv_count.load();

    server.resume();
    CHECK(!server.paused());
    platform_sleep_ms(50);

    sender.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(port));

    for (int i = 0; i < 100 && recv_count.load() <= count_at_pause; ++i)
        platform_sleep_ms(10);

    CHECK_MSG(recv_count.load() > count_at_pause, "should receive message after resume");
}

// ===========================================================================
// dgram6 batch send
// ===========================================================================

void test_dgram6_batch_send()
{
    const uint16_t port = 29883;
    const int num_messages = 5;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());
    std::atomic<int> recv_count {0};

    server.on_data([&recv_count](const char *, size_t, const socketpp::inet6_address &)
                   { recv_count.fetch_add(1, std::memory_order_relaxed); });

    auto sender_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(sender_r, "sender create failed");
    if (!sender_r)
        return;

    auto sender = std::move(sender_r.value());

    platform_sleep_ms(50);

    std::vector<std::string> payloads;
    std::vector<socketpp::dgram6_send_entry> batch;
    auto dest = socketpp::inet6_address::loopback(port);

    payloads.reserve(num_messages);

    for (int i = 0; i < num_messages; ++i)
        payloads.push_back("batch-v6-" + std::to_string(i));

    for (int i = 0; i < num_messages; ++i)
        batch.push_back({payloads[i].data(), payloads[i].size(), dest});

    auto r = sender.send_batch(socketpp::span<const socketpp::dgram6_send_entry>(batch.data(), batch.size()));
    CHECK_MSG(r, "batch send should succeed");

    auto ok = wait_for_count(recv_count, num_messages, 10000);
    CHECK_MSG(ok, "not all batch messages received");
}

// ===========================================================================
// dgram6 claim basic
// ===========================================================================

void test_dgram6_claim_basic()
{
    const uint16_t server_port = 29888;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

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

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet6_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(100);

    const std::string msg = "from-claimed-v6";
    client.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(server_port));

    auto ok = wait_for_count(peer_recv_count, 1, 5000);
    CHECK_MSG(ok, "peer handle should receive datagram from claimed address");

    platform_sleep_ms(100);
    CHECK_MSG(main_recv_count.load() == 0, "main on_data should NOT fire for claimed peer");
}

// ===========================================================================
// dgram6 peer send
// ===========================================================================

void test_dgram6_peer_send()
{
    const uint16_t server_port = 29889;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<bool> client_recv {false};
    std::string received_data;
    std::mutex recv_mutex;

    client.on_data(
        [&](const char *data, size_t len, const socketpp::inet6_address &)
        {
            std::lock_guard<std::mutex> lock(recv_mutex);
            received_data.assign(data, len);
            client_recv.store(true, std::memory_order_relaxed);
        });

    auto peer_r = server.claim(client_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());
    peer.on_data([](const char *, size_t) {});
    platform_sleep_ms(100);

    const std::string msg = "from-peer-handle-v6";
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
// dgram6 peer relinquish
// ===========================================================================

void test_dgram6_peer_relinquish()
{
    const uint16_t server_port = 29890;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<int> peer_recv_count {0};
    std::atomic<int> main_recv_count {0};

    auto peer_r = server.claim(client_addr);
    CHECK_MSG(peer_r, "claim should succeed");
    if (!peer_r)
        return;

    auto peer = std::move(peer_r.value());
    peer.on_data([&peer_recv_count](const char *, size_t) { peer_recv_count.fetch_add(1, std::memory_order_relaxed); });

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet6_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    platform_sleep_ms(100);

    const std::string msg = "before-relinquish-v6";
    client.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(server_port));

    wait_for_count(peer_recv_count, 1, 5000);
    CHECK_MSG(peer_recv_count.load() >= 1, "peer should receive before relinquish");

    peer.relinquish();
    CHECK(!peer.is_open());
    platform_sleep_ms(200);

    const std::string msg2 = "after-relinquish-v6";
    client.send_to(msg2.data(), msg2.size(), socketpp::inet6_address::loopback(server_port));

    auto ok = wait_for_count(main_recv_count, 1, 5000);
    CHECK_MSG(ok, "main on_data should receive after relinquish");
}

// ===========================================================================
// dgram6 peer destructor auto-relinquish
// ===========================================================================

void test_dgram6_peer_destructor_relinquish()
{
    const uint16_t server_port = 29891;

    auto server_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(server_port));
    if (!server_r)
    {
        std::cerr << "(port in use or IPv6 unavailable, skipping) ";
        CHECK(true);
        return;
    }

    auto server = std::move(server_r.value());

    auto client_r = socketpp::dgram6::create(socketpp::inet6_address::loopback(0));
    CHECK_MSG(client_r, "client create failed");
    if (!client_r)
        return;

    auto client = std::move(client_r.value());
    auto client_addr = client.local_addr();

    std::atomic<int> main_recv_count {0};

    server.on_data([&main_recv_count](const char *, size_t, const socketpp::inet6_address &)
                   { main_recv_count.fetch_add(1, std::memory_order_relaxed); });

    {
        auto peer_r = server.claim(client_addr);
        CHECK_MSG(peer_r, "claim should succeed");
        if (!peer_r)
            return;

        auto peer = std::move(peer_r.value());
        peer.on_data([](const char *, size_t) {});
        platform_sleep_ms(100);
    }

    platform_sleep_ms(200);

    const std::string msg = "after-destroy-v6";
    client.send_to(msg.data(), msg.size(), socketpp::inet6_address::loopback(server_port));

    auto ok = wait_for_count(main_recv_count, 1, 5000);
    CHECK_MSG(ok, "main on_data should receive after peer destructor");
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "IPv6 high-level API tests\n";

    std::cerr << "\n--- stream6 echo ---\n";
    RUN_TEST(test_stream6_echo_round_trip);

    std::cerr << "\n--- stream6 multi client ---\n";
    RUN_TEST(test_stream6_multi_client);

    std::cerr << "\n--- stream6 close from server ---\n";
    RUN_TEST(test_stream6_close_from_server);

    std::cerr << "\n--- stream6 connection pause/resume ---\n";
    RUN_TEST(test_stream6_connection_pause_resume);

    std::cerr << "\n--- dgram6 echo ---\n";
    RUN_TEST(test_dgram6_echo_round_trip);

    std::cerr << "\n--- dgram6 ephemeral port ---\n";
    RUN_TEST(test_dgram6_ephemeral_port);

    std::cerr << "\n--- dgram6 pause/resume ---\n";
    RUN_TEST(test_dgram6_pause_resume);

    std::cerr << "\n--- dgram6 batch send ---\n";
    RUN_TEST(test_dgram6_batch_send);

    std::cerr << "\n--- dgram6 claim basic ---\n";
    RUN_TEST(test_dgram6_claim_basic);

    std::cerr << "\n--- dgram6 peer send ---\n";
    RUN_TEST(test_dgram6_peer_send);

    std::cerr << "\n--- dgram6 peer relinquish ---\n";
    RUN_TEST(test_dgram6_peer_relinquish);

    std::cerr << "\n--- dgram6 peer destructor relinquish ---\n";
    RUN_TEST(test_dgram6_peer_destructor_relinquish);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

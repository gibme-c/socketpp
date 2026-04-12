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

/// @file tcp_throughput_bench.cpp
/// @brief TCP loopback throughput benchmark for stream4.
///
/// Blasts data over a loopback TCP connection for a fixed duration and reports
/// receive throughput in Mbps. Useful for verifying event-driven TCP dispatch
/// performance across platforms (epoll, kqueue, IOCP).
///
/// Usage:
///   tcp_throughput_bench [duration_secs] [chunk_bytes]
///
/// Defaults: 3 seconds, 65536 bytes per send chunk.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <socketpp.hpp>
#include <thread>

int main(int argc, char *argv[])
{
    const int duration_secs = (argc > 1) ? std::atoi(argv[1]) : 3;
    const size_t chunk_size = (argc > 2) ? static_cast<size_t>(std::atoi(argv[2])) : 65536;

    if (duration_secs <= 0 || chunk_size == 0 || chunk_size > 16 * 1024 * 1024)
    {
        std::fprintf(stderr, "usage: %s [duration_secs] [chunk_bytes]\n", argv[0]);
        std::fprintf(stderr, "  duration_secs: 1-%d (default 3)\n", 60);
        std::fprintf(stderr, "  chunk_bytes: 1-16777216 (default 65536)\n");
        return 1;
    }

    constexpr uint16_t port = 19951;

    // ── Server ──────────────────────────────────────────────────────────

    auto server_r = socketpp::stream4::listen(socketpp::inet4_address::loopback(port));

    if (!server_r)
    {
        std::fprintf(stderr, "server listen failed: %s\n", server_r.message().c_str());
        return 1;
    }

    auto server = std::move(server_r.value());

    std::atomic<uint64_t> total_bytes {0};

    server.on_connect(
        [&](socketpp::stream4::connection &conn)
        { conn.on_data([&](const char *, size_t len) { total_bytes.fetch_add(len, std::memory_order_relaxed); }); });

    // ── Client ──────────────────────────────────────────────────────────

    auto client_r = socketpp::stream4::connect(socketpp::inet4_address::loopback(port));

    if (!client_r)
    {
        std::fprintf(stderr, "client connect failed: %s\n", client_r.message().c_str());
        return 1;
    }

    auto client = std::move(client_r.value());

    std::atomic<bool> connected {false};
    socketpp::stream4::connection *client_conn_ptr = nullptr;

    client.on_connect(
        [&](socketpp::stream4::connection &conn)
        {
            client_conn_ptr = &conn;
            connected.store(true, std::memory_order_release);
            conn.on_data([](const char *, size_t) {});
        });

    // Wait for the connection to establish.
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (!connected.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                std::fprintf(stderr, "connection timeout\n");
                return 1;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Let event loops settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<char> payload(chunk_size, 'X');

    std::fprintf(
        stderr,
        "TCP throughput bench: %zu-byte chunks, %d second%s, loopback\n",
        chunk_size,
        duration_secs,
        duration_secs == 1 ? "" : "s");

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(duration_secs);

    uint64_t sent_bytes = 0;
    uint64_t send_failures = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (client_conn_ptr->send(payload.data(), payload.size()))
            sent_bytes += payload.size();
        else
            ++send_failures;

        // Back off briefly if the write queue is deep to avoid unbounded memory growth.
        if (client_conn_ptr->write_queue_bytes() > 4 * 1024 * 1024)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Let receiver drain remaining data.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double secs = std::chrono::duration<double>(elapsed).count();

    const uint64_t rx_bytes = total_bytes.load(std::memory_order_relaxed);

    const double rx_mbps = (rx_bytes * 8.0) / (secs * 1'000'000.0);
    const double tx_mbps = (sent_bytes * 8.0) / (secs * 1'000'000.0);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  duration:       %.2f s\n", secs);
    std::fprintf(
        stderr, "  sent:           %llu bytes (%.1f Mbps)\n", static_cast<unsigned long long>(sent_bytes), tx_mbps);
    std::fprintf(stderr, "  send failures:  %llu\n", static_cast<unsigned long long>(send_failures));
    std::fprintf(
        stderr, "  received:       %llu bytes (%.1f Mbps)\n", static_cast<unsigned long long>(rx_bytes), rx_mbps);
    std::fprintf(stderr, "  throughput:     %.1f Mbps\n", rx_mbps);

    return 0;
}

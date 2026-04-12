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

/// @file udp_throughput_bench.cpp
/// @brief UDP loopback throughput benchmark for dgram4.
///
/// Blasts 1400-byte datagrams over loopback for a fixed duration and reports
/// receive throughput in Mbps. Useful for verifying event-driven UDP dispatch
/// performance across platforms (epoll, kqueue, IOCP).
///
/// Usage:
///   udp_throughput_bench [duration_secs] [payload_bytes]
///
/// Defaults: 3 seconds, 1400 bytes per datagram.

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
    const size_t payload_size = (argc > 2) ? static_cast<size_t>(std::atoi(argv[2])) : 1400;

    if (duration_secs <= 0 || payload_size == 0 || payload_size > 65507)
    {
        std::fprintf(stderr, "usage: %s [duration_secs] [payload_bytes]\n", argv[0]);
        std::fprintf(stderr, "  duration_secs: 1-%d (default 3)\n", 60);
        std::fprintf(stderr, "  payload_bytes: 1-65507 (default 1400)\n");
        return 1;
    }

    // ── Receiver ──────────────────────────────────────────────────────────

    constexpr uint16_t port = 19950;

    auto rx_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(port));

    if (!rx_r)
    {
        std::fprintf(stderr, "receiver create failed: %s\n", rx_r.message().c_str());
        return 1;
    }

    auto rx = std::move(rx_r.value());

    std::atomic<uint64_t> total_bytes {0};
    std::atomic<uint64_t> total_packets {0};

    rx.on_data(
        [&](const char *, size_t len, const socketpp::inet4_address &)
        {
            total_bytes.fetch_add(len, std::memory_order_relaxed);
            total_packets.fetch_add(1, std::memory_order_relaxed);
        });

    // ── Sender ────────────────────────────────────────────────────────────

    auto tx_r = socketpp::dgram4::create(socketpp::inet4_address::loopback(0));

    if (!tx_r)
    {
        std::fprintf(stderr, "sender create failed: %s\n", tx_r.message().c_str());
        return 1;
    }

    auto tx = std::move(tx_r.value());

    // Let event loops settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<char> payload(payload_size, 'X');
    const auto dest = socketpp::inet4_address::loopback(port);

    std::fprintf(
        stderr,
        "UDP throughput bench: %zu-byte datagrams, %d second%s, loopback\n",
        payload_size,
        duration_secs,
        duration_secs == 1 ? "" : "s");

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(duration_secs);

    uint64_t sent_packets = 0;
    uint64_t send_failures = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < 100; ++i)
        {
            if (tx.send_to(payload.data(), payload.size(), dest))
                ++sent_packets;
            else
                ++send_failures;
        }
    }

    // Let receiver drain remaining datagrams.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double secs = std::chrono::duration<double>(elapsed).count();

    const uint64_t rx_bytes = total_bytes.load(std::memory_order_relaxed);
    const uint64_t rx_pkts = total_packets.load(std::memory_order_relaxed);

    const double rx_mbps = (rx_bytes * 8.0) / (secs * 1'000'000.0);
    const double tx_rate = sent_packets / secs;
    const double rx_rate = rx_pkts / secs;
    const double loss_pct = sent_packets > 0 ? (1.0 - static_cast<double>(rx_pkts) / sent_packets) * 100.0 : 0.0;

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  duration:       %.2f s\n", secs);
    std::fprintf(
        stderr,
        "  sent:           %llu packets (%.0f pkt/s)\n",
        static_cast<unsigned long long>(sent_packets),
        tx_rate);
    std::fprintf(stderr, "  send failures:  %llu\n", static_cast<unsigned long long>(send_failures));
    std::fprintf(
        stderr, "  received:       %llu packets (%.0f pkt/s)\n", static_cast<unsigned long long>(rx_pkts), rx_rate);
    std::fprintf(stderr, "  received bytes: %llu\n", static_cast<unsigned long long>(rx_bytes));
    std::fprintf(stderr, "  throughput:     %.1f Mbps\n", rx_mbps);
    std::fprintf(stderr, "  loss:           %.1f%%\n", loss_pct);

    return 0;
}

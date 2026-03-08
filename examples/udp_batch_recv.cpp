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

/// @file udp_batch_recv.cpp
/// Demonstrates receiving UDP datagrams using the high-level dgram API.
/// Sets up IPv4 (127.0.0.1:9002) and IPv6 ([::1]:9062) receivers that
/// print incoming datagrams. Also demonstrates using a second dgram4/dgram6
/// instance to send test datagrams.

#include <atomic>
#include <iostream>
#include <socketpp.hpp>
#include <string>
#include <thread>

int main()
{
    // ── IPv4 datagram receiver ─────────────────────────────────────────

    {
        std::atomic<int> received {0};
        constexpr int expected = 5;

        auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(9002));

        if (!r)
        {
            std::cerr << "IPv4 failed to create: " << r.message() << "\n";
        }
        else
        {
            auto receiver = std::move(r.value());

            std::cout << "IPv4 receiver listening on " << receiver.local_addr().to_string() << "\n";

            receiver.on_data(
                [&received](const char *data, size_t len, const socketpp::inet4_address &from)
                {
                    std::string msg(data, len);
                    auto idx = received.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "  IPv4 [" << idx << "] " << len << " bytes from " << from.to_string() << ": " << msg
                              << "\n";
                });

            // Send some test datagrams using a second dgram4 as client
            auto sender_r = socketpp::dgram4::create();

            if (!sender_r)
            {
                std::cerr << "IPv4 sender failed to create: " << sender_r.message() << "\n";
            }
            else
            {
                auto sender = std::move(sender_r.value());
                socketpp::inet4_address dest("127.0.0.1", 9002);

                for (int i = 0; i < expected; ++i)
                {
                    std::string msg = "IPv4 message #" + std::to_string(i);
                    sender.send_to(msg.data(), msg.size(), dest);
                }

                // Wait for all datagrams to arrive
                for (int i = 0; i < 200 && received.load(std::memory_order_relaxed) < expected; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                std::cout << "IPv4 received " << received.load(std::memory_order_relaxed) << " of " << expected
                          << " datagrams\n";
            }
        }
    }

    // ── IPv6 datagram receiver ─────────────────────────────────────────

    {
        std::atomic<int> received {0};
        constexpr int expected = 5;

        auto r = socketpp::dgram6::create(socketpp::inet6_address::loopback(9062));

        if (!r)
        {
            std::cerr << "IPv6 failed to create: " << r.message() << "\n";
        }
        else
        {
            auto receiver = std::move(r.value());

            std::cout << "IPv6 receiver listening on " << receiver.local_addr().to_string() << "\n";

            receiver.on_data(
                [&received](const char *data, size_t len, const socketpp::inet6_address &from)
                {
                    std::string msg(data, len);
                    auto idx = received.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "  IPv6 [" << idx << "] " << len << " bytes from " << from.to_string() << ": " << msg
                              << "\n";
                });

            // Send some test datagrams using a second dgram6 as client
            auto sender_r = socketpp::dgram6::create();

            if (!sender_r)
            {
                std::cerr << "IPv6 sender failed to create: " << sender_r.message() << "\n";
            }
            else
            {
                auto sender = std::move(sender_r.value());
                socketpp::inet6_address dest = socketpp::inet6_address::loopback(9062);

                for (int i = 0; i < expected; ++i)
                {
                    std::string msg = "IPv6 message #" + std::to_string(i);
                    sender.send_to(msg.data(), msg.size(), dest);
                }

                // Wait for all datagrams to arrive
                for (int i = 0; i < 200 && received.load(std::memory_order_relaxed) < expected; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                std::cout << "IPv6 received " << received.load(std::memory_order_relaxed) << " of " << expected
                          << " datagrams\n";
            }
        }
    }

    return 0;
}

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

/// @file tcp_echo_client.cpp
/// TCP echo client using the high-level socketpp stream API.
/// Demonstrates both IPv4 (127.0.0.1:9000) and IPv6 ([::1]:9060) connections.

#include <atomic>
#include <iostream>
#include <socketpp.hpp>
#include <string>
#include <thread>

int main()
{
    // ── IPv4 client ─────────────────────────────────────────────────────

    {
        std::atomic<bool> done {false};

        auto r4 = socketpp::stream4::connect(socketpp::inet4_address::loopback(9000));

        if (!r4)
        {
            std::cerr << "IPv4 connect setup failed: " << r4.message() << "\n";
            return 1;
        }

        auto client4 = std::move(r4.value());

        client4
            .on_error(
                [&done](std::error_code ec)
                {
                    std::cerr << "IPv4 connect failed: " << ec.message() << "\n";
                    done.store(true, std::memory_order_relaxed);
                })
            .on_connect(
                [&done](socketpp::stream4::connection &conn)
                {
                    std::cout << "IPv4 connected to " << conn.peer_addr().to_string() << "\n";

                    const std::string msg = "Hello from socketpp IPv4 client!";
                    conn.send(msg);
                    std::cout << "IPv4 sent: " << msg << "\n";

                    conn.on_data(
                        [&done](const char *data, size_t len)
                        {
                            std::string echo(data, len);
                            std::cout << "IPv4 received echo: " << echo << "\n";
                            done.store(true, std::memory_order_relaxed);
                        });
                });

        while (!done.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ── IPv6 client ─────────────────────────────────────────────────────

    {
        std::atomic<bool> done {false};

        auto r6 = socketpp::stream6::connect(socketpp::inet6_address::loopback(9060));

        if (!r6)
        {
            std::cerr << "IPv6 connect setup failed: " << r6.message() << "\n";
            return 1;
        }

        auto client6 = std::move(r6.value());

        client6
            .on_error(
                [&done](std::error_code ec)
                {
                    std::cerr << "IPv6 connect failed: " << ec.message() << "\n";
                    done.store(true, std::memory_order_relaxed);
                })
            .on_connect(
                [&done](socketpp::stream6::connection &conn)
                {
                    std::cout << "IPv6 connected to " << conn.peer_addr().to_string() << "\n";

                    const std::string msg = "Hello from socketpp IPv6 client!";
                    conn.send(msg);
                    std::cout << "IPv6 sent: " << msg << "\n";

                    conn.on_data(
                        [&done](const char *data, size_t len)
                        {
                            std::string echo(data, len);
                            std::cout << "IPv6 received echo: " << echo << "\n";
                            done.store(true, std::memory_order_relaxed);
                        });
                });

        while (!done.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}

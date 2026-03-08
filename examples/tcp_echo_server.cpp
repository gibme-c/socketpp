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

/// @file tcp_echo_server.cpp
/// TCP echo server using the high-level socketpp API.
/// Demonstrates both IPv4 (127.0.0.1:9000) and IPv6 ([::1]:9060) servers.

#include <iostream>
#include <socketpp.hpp>
#include <thread>

int main()
{
    // ── IPv4 echo server ────────────────────────────────────────────────

    socketpp::tcp4_server server4;

    server4.on_connect(
        [](socketpp::tcp4_connection &conn)
        {
            std::cout << "IPv4 client connected: " << conn.peer_addr().to_string() << "\n";

            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });

            conn.on_close([]() { std::cout << "IPv4 client disconnected\n"; });
        });

    auto r4 = server4.listen(socketpp::inet4_address::loopback(9000));

    if (!r4)
    {
        std::cerr << "IPv4 listen failed: " << r4.message() << "\n";
        return 1;
    }

    std::cout << "IPv4 echo server listening on 127.0.0.1:9000\n";
    server4.start();

    // ── IPv6 echo server ────────────────────────────────────────────────

    socketpp::tcp6_server server6;

    server6.on_connect(
        [](socketpp::tcp6_connection &conn)
        {
            std::cout << "IPv6 client connected: " << conn.peer_addr().to_string() << "\n";

            conn.on_data([&conn](const char *data, size_t len) { conn.send(data, len); });

            conn.on_close([]() { std::cout << "IPv6 client disconnected\n"; });
        });

    auto r6 = server6.listen(socketpp::inet6_address::loopback(9060));

    if (!r6)
    {
        std::cerr << "IPv6 listen failed: " << r6.message() << "\n";
        server4.stop();
        return 1;
    }

    std::cout << "IPv6 echo server listening on [::1]:9060\n";
    std::cout << "Shutting down in 60 seconds...\n";
    server6.start();

    // Run for 60 seconds then shut down
    std::this_thread::sleep_for(std::chrono::seconds(60));

    server4.stop();
    server6.stop();

    std::cout << "Shutdown complete\n";
    return 0;
}

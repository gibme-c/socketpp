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

/// @file udp_echo_server.cpp
/// UDP echo server using the high-level socketpp dgram API.
/// Demonstrates both IPv4 (127.0.0.1:9001) and IPv6 ([::1]:9061) servers.

#include <iostream>
#include <socketpp.hpp>
#include <thread>

int main()
{
    // ── IPv4 UDP echo server ────────────────────────────────────────────

    auto r4 = socketpp::dgram4::create(socketpp::inet4_address::loopback(9001));

    if (!r4)
    {
        std::cerr << "IPv4 create failed: " << r4.message() << "\n";
        return 1;
    }

    auto server4 = std::move(r4.value());

    server4.on_data(
        [&server4](const char *data, size_t len, const socketpp::inet4_address &from)
        {
            std::cout << "IPv4 received " << len << " bytes from " << from.to_string() << "\n";
            server4.send_to(data, len, from);
        });

    std::cout << "IPv4 UDP echo server listening on 127.0.0.1:9001\n";

    // ── IPv6 UDP echo server ────────────────────────────────────────────

    auto r6 = socketpp::dgram6::create(socketpp::inet6_address::loopback(9061));

    if (!r6)
    {
        std::cerr << "IPv6 create failed: " << r6.message() << "\n";
        return 1;
    }

    auto server6 = std::move(r6.value());

    server6.on_data(
        [&server6](const char *data, size_t len, const socketpp::inet6_address &from)
        {
            std::cout << "IPv6 received " << len << " bytes from " << from.to_string() << "\n";
            server6.send_to(data, len, from);
        });

    std::cout << "IPv6 UDP echo server listening on [::1]:9061\n";
    std::cout << "Shutting down in 60 seconds...\n";

    // Run for 60 seconds then shut down (destructors handle cleanup)
    std::this_thread::sleep_for(std::chrono::seconds(60));

    std::cout << "Shutdown complete\n";
    return 0;
}

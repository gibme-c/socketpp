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
/// Demonstrates batch receive with socketpp.
/// recv_batch abstracts platform differences: uses recvmmsg on Linux,
/// falls back to a tight recv_from loop elsewhere. Callers just call recv_batch.
/// Demonstrates both IPv4 (127.0.0.1:9002) and IPv6 ([::1]:9062) usage.

#include <array>
#include <iostream>
#include <socketpp.hpp>
#include <string>

int main()
{
    auto opts = socketpp::socket_options {}.reuse_addr(true);

    // ── IPv4 batch receive ───────────────────────────────────────────────

    {
        socketpp::udp4_socket sock;
        auto r = sock.open(socketpp::inet4_address::loopback(9002), opts);

        if (!r)
        {
            std::cerr << "IPv4 failed to open: " << r.message() << "\n";
        }
        else
        {
            std::cout << "IPv4 batch receiver listening on " << socketpp::inet4_address::loopback(9002).to_string()
                      << "\n";

            constexpr int batch_size = 16;
            constexpr int buf_len = 1500;

            std::array<std::array<char, buf_len>, batch_size> buffers {};
            std::array<socketpp::msg_batch_entry, batch_size> entries {};

            for (int i = 0; i < batch_size; ++i)
            {
                entries[i].buf = buffers[i].data();
                entries[i].len = buf_len;
            }

            auto batch_r = sock.recv_batch(socketpp::span<socketpp::msg_batch_entry>(entries.data(), batch_size));

            if (!batch_r)
            {
                std::cerr << "IPv4 recv_batch failed: " << batch_r.message() << "\n";
            }
            else
            {
                auto count = batch_r.value();
                std::cout << "IPv4 received " << count << " datagrams in one batch call:\n";

                for (int i = 0; i < count; ++i)
                {
                    std::string msg(static_cast<const char *>(entries[i].buf), entries[i].transferred);
                    std::cout << "  [" << i << "] " << entries[i].transferred << " bytes: " << msg << "\n";
                }
            }

            sock.close();
        }
    }

    // ── IPv6 batch receive ───────────────────────────────────────────────

    {
        socketpp::udp6_socket sock;
        auto r = sock.open(socketpp::inet6_address::loopback(9062), opts);

        if (!r)
        {
            std::cerr << "IPv6 failed to open: " << r.message() << "\n";
        }
        else
        {
            std::cout << "IPv6 batch receiver listening on " << socketpp::inet6_address::loopback(9062).to_string()
                      << "\n";

            constexpr int batch_size = 16;
            constexpr int buf_len = 1500;

            std::array<std::array<char, buf_len>, batch_size> buffers {};
            std::array<socketpp::msg_batch_entry, batch_size> entries {};

            for (int i = 0; i < batch_size; ++i)
            {
                entries[i].buf = buffers[i].data();
                entries[i].len = buf_len;
            }

            auto batch_r = sock.recv_batch(socketpp::span<socketpp::msg_batch_entry>(entries.data(), batch_size));

            if (!batch_r)
            {
                std::cerr << "IPv6 recv_batch failed: " << batch_r.message() << "\n";
            }
            else
            {
                auto count = batch_r.value();
                std::cout << "IPv6 received " << count << " datagrams in one batch call:\n";

                for (int i = 0; i < count; ++i)
                {
                    std::string msg(static_cast<const char *>(entries[i].buf), entries[i].transferred);
                    std::cout << "  [" << i << "] " << entries[i].transferred << " bytes: " << msg << "\n";
                }
            }

            sock.close();
        }
    }

    return 0;
}

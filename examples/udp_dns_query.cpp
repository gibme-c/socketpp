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

/// @file udp_dns_query.cpp
/// UDP client example: sends a DNS A-record query (RFC 1035) to a public resolver
/// and parses the response. Demonstrates using dgram4 as a client to communicate
/// with a well-known internet service via the high-level API.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <socketpp.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Encode a domain name into DNS wire format (sequence of length-prefixed labels)
    std::vector<uint8_t> encode_dns_name(const std::string &domain)
    {
        std::vector<uint8_t> result;
        std::istringstream stream(domain);
        std::string label;

        while (std::getline(stream, label, '.'))
        {
            result.push_back(static_cast<uint8_t>(label.size()));
            result.insert(result.end(), label.begin(), label.end());
        }

        result.push_back(0); // root label
        return result;
    }

    // Build a DNS query packet for an A record
    std::vector<uint8_t> build_dns_query(const std::string &domain, uint16_t query_id)
    {
        std::vector<uint8_t> packet;
        packet.reserve(64);

        // DNS Header (12 bytes)
        // ID
        packet.push_back(static_cast<uint8_t>(query_id >> 8));
        packet.push_back(static_cast<uint8_t>(query_id & 0xFF));
        // Flags: standard query, recursion desired
        packet.push_back(0x01); // QR=0, Opcode=0, AA=0, TC=0, RD=1
        packet.push_back(0x00); // RA=0, Z=0, RCODE=0
        // QDCOUNT = 1
        packet.push_back(0x00);
        packet.push_back(0x01);
        // ANCOUNT = 0
        packet.push_back(0x00);
        packet.push_back(0x00);
        // NSCOUNT = 0
        packet.push_back(0x00);
        packet.push_back(0x00);
        // ARCOUNT = 0
        packet.push_back(0x00);
        packet.push_back(0x00);

        // Question section
        auto name = encode_dns_name(domain);
        packet.insert(packet.end(), name.begin(), name.end());

        // QTYPE = A (1)
        packet.push_back(0x00);
        packet.push_back(0x01);
        // QCLASS = IN (1)
        packet.push_back(0x00);
        packet.push_back(0x01);

        return packet;
    }

    uint16_t read_u16(const uint8_t *p)
    {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    // Skip a DNS name in the response (handles compression pointers)
    size_t skip_dns_name(const uint8_t *buf, size_t offset, size_t buf_len)
    {
        while (offset < buf_len)
        {
            uint8_t len = buf[offset];

            if (len == 0)
            {
                return offset + 1;
            }

            // Compression pointer (top two bits set)
            if ((len & 0xC0) == 0xC0)
            {
                return offset + 2;
            }

            offset += 1 + len;
        }

        return offset;
    }

    struct dns_answer
    {
        uint16_t type;
        uint16_t cls;
        uint32_t ttl;
        std::vector<uint8_t> rdata;
    };

    // Parse answer records from a DNS response
    std::vector<dns_answer> parse_dns_response(const uint8_t *buf, size_t len, uint16_t expected_id)
    {
        std::vector<dns_answer> answers;

        if (len < 12)
        {
            std::cerr << "Response too short for DNS header\n";
            return answers;
        }

        uint16_t resp_id = read_u16(buf);

        if (resp_id != expected_id)
        {
            std::cerr << "ID mismatch: expected 0x" << std::hex << expected_id << " got 0x" << resp_id << std::dec
                      << "\n";
            return answers;
        }

        uint8_t rcode = buf[3] & 0x0F;

        if (rcode != 0)
        {
            std::cerr << "DNS error, RCODE=" << static_cast<int>(rcode) << "\n";
            return answers;
        }

        uint16_t qdcount = read_u16(buf + 4);
        uint16_t ancount = read_u16(buf + 6);

        // Skip question section
        size_t offset = 12;

        for (uint16_t i = 0; i < qdcount && offset < len; ++i)
        {
            offset = skip_dns_name(buf, offset, len);
            offset += 4; // QTYPE + QCLASS
        }

        // Parse answer records
        for (uint16_t i = 0; i < ancount && offset < len; ++i)
        {
            offset = skip_dns_name(buf, offset, len);

            if (offset + 10 > len)
            {
                break;
            }

            dns_answer ans;
            ans.type = read_u16(buf + offset);
            ans.cls = read_u16(buf + offset + 2);
            ans.ttl = static_cast<uint32_t>(
                (buf[offset + 4] << 24) | (buf[offset + 5] << 16) | (buf[offset + 6] << 8) | buf[offset + 7]);
            uint16_t rdlength = read_u16(buf + offset + 8);
            offset += 10;

            if (offset + rdlength > len)
            {
                break;
            }

            ans.rdata.assign(buf + offset, buf + offset + rdlength);
            offset += rdlength;
            answers.push_back(ans);
        }

        return answers;
    }

    void print_answers(const std::vector<dns_answer> &answers)
    {
        if (answers.empty())
        {
            std::cout << "No answer records found\n";
        }
        else
        {
            for (const auto &ans : answers)
            {
                if (ans.type == 1 && ans.rdata.size() == 4) // A record
                {
                    std::cout << "  A " << static_cast<int>(ans.rdata[0]) << "." << static_cast<int>(ans.rdata[1])
                              << "." << static_cast<int>(ans.rdata[2]) << "." << static_cast<int>(ans.rdata[3])
                              << " (TTL " << ans.ttl << "s)\n";
                }
                else if (ans.type == 5) // CNAME
                {
                    std::cout << "  CNAME (TTL " << ans.ttl << "s)\n";
                }
                else
                {
                    std::cout << "  type=" << ans.type << " rdlen=" << ans.rdata.size() << " (TTL " << ans.ttl
                              << "s)\n";
                }
            }
        }
    }

    void query_dns(const std::string &domain, const std::string &server_ip)
    {
        const uint16_t query_id = 0xABCD;

        auto packet = build_dns_query(domain, query_id);

        // Create a dgram4 bound to an ephemeral port
        auto r = socketpp::dgram4::create();

        if (!r)
        {
            std::cerr << "Failed to create dgram4: " << r.message() << "\n";
            return;
        }

        auto sock = std::move(r.value());

        std::atomic<bool> done {false};
        std::mutex output_mutex;

        sock.on_data(
            [&](const char *data, size_t len, const socketpp::inet4_address &from)
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "Received " << len << " bytes from " << from.to_string() << "\n";

                auto answers = parse_dns_response(reinterpret_cast<const uint8_t *>(data), len, query_id);
                print_answers(answers);
                done.store(true, std::memory_order_relaxed);
            });

        socketpp::inet4_address dest(server_ip, 53);

        std::cout << "Querying " << server_ip << " for A record of " << domain << "...\n";

        if (!sock.send_to(packet.data(), packet.size(), dest))
        {
            std::cerr << "send_to failed\n";
            return;
        }

        // Wait for the response (with a timeout)
        for (int i = 0; i < 500 && !done.load(std::memory_order_relaxed); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!done.load(std::memory_order_relaxed))
        {
            std::cerr << "Timed out waiting for DNS response\n";
        }
    }
} // namespace

int main()
{
    std::cout << "=== DNS Query via Google Public DNS (8.8.8.8) ===\n";
    query_dns("example.com", "8.8.8.8");

    std::cout << "\n=== DNS Query via Cloudflare DNS (1.1.1.1) ===\n";
    query_dns("example.com", "1.1.1.1");

    std::cout << "\n=== DNS Query for multi-record domain ===\n";
    query_dns("dns.google", "8.8.8.8");

    return 0;
}

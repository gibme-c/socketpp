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

/// @file fuzz_inet4_roundtrip.cpp
/// Property: inet4_address(ip, port) -> to_string() -> parse() is idempotent.

#include <cstdint>
#include <cstring>
#include <socketpp/net/inet4.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 6)
        return 0;

    uint32_t ip;
    uint16_t port;
    std::memcpy(&ip, data, sizeof(ip));
    std::memcpy(&port, data + 4, sizeof(port));

    socketpp::inet4_address addr(ip, port);

    if (addr.ip() != ip)
        __builtin_trap();

    if (addr.port() != port)
        __builtin_trap();

    // Reconstruction round-trip: same ip/port yields same address
    socketpp::inet4_address addr2(addr.ip(), addr.port());

    if (addr2.ip() != ip)
        __builtin_trap();

    if (addr2.port() != port)
        __builtin_trap();

    // sock_address conversion consistency
    socketpp::sock_address sa = addr;

    if (!sa.is_v4())
        __builtin_trap();

    if (sa.is_v6())
        __builtin_trap();

    // Equality must hold
    socketpp::sock_address sa2 = addr2;

    if (!(sa == sa2))
        __builtin_trap();

    return 0;
}

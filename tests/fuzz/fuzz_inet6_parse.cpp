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

/// @file fuzz_inet6_parse.cpp
/// Property: inet6_address::parse() never crashes on arbitrary input.
/// On success, the address bytes round-trip through the constructor.

#include <cstdint>
#include <cstring>
#include <socketpp/net/inet6.hpp>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3)
        return 0;

    // Use first 2 bytes as port, next byte as scope_id low byte
    uint16_t port;
    std::memcpy(&port, data, sizeof(port));
    uint32_t scope_id = data[2];

    std::string_view input(reinterpret_cast<const char *>(data + 3), size - 3);

    auto r = socketpp::inet6_address::parse(input, port, scope_id);

    if (!r)
        return 0;

    const auto &addr = r.value();

    // Round-trip: extract bytes and reconstruct
    uint8_t bytes[16];
    addr.bytes(bytes);

    socketpp::inet6_address reconstructed(bytes, addr.port(), addr.scope_id(), addr.flowinfo());

    uint8_t bytes2[16];
    reconstructed.bytes(bytes2);

    if (std::memcmp(bytes, bytes2, 16) != 0)
        __builtin_trap();

    if (reconstructed.port() != addr.port())
        __builtin_trap();

    return 0;
}

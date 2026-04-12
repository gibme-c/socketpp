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

/// @file fuzz_inet6_roundtrip.cpp
/// Property: inet6_address(bytes, port, scope_id, flowinfo) round-trips
/// through bytes()/port()/scope_id()/flowinfo() accessors.

#include <cstdint>
#include <cstring>
#include <socketpp/net/inet6.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    // 16 (addr) + 2 (port) + 4 (scope_id) + 4 (flowinfo) = 26
    if (size < 26)
        return 0;

    uint8_t addr_bytes[16];
    std::memcpy(addr_bytes, data, 16);

    uint16_t port;
    std::memcpy(&port, data + 16, sizeof(port));

    uint32_t scope_id;
    std::memcpy(&scope_id, data + 18, sizeof(scope_id));

    uint32_t flowinfo;
    std::memcpy(&flowinfo, data + 22, sizeof(flowinfo));

    socketpp::inet6_address addr(addr_bytes, port, scope_id, flowinfo);

    // Verify accessors round-trip
    uint8_t out_bytes[16];
    addr.bytes(out_bytes);

    if (std::memcmp(addr_bytes, out_bytes, 16) != 0)
        __builtin_trap();

    if (addr.port() != port)
        __builtin_trap();

    if (addr.scope_id() != scope_id)
        __builtin_trap();

    if (addr.flowinfo() != flowinfo)
        __builtin_trap();

    // sock_address conversion consistency
    socketpp::sock_address sa = addr;

    if (!sa.is_v6())
        __builtin_trap();

    if (sa.is_v4())
        __builtin_trap();

    return 0;
}

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

/// @file fuzz_sock_address_roundtrip.cpp
/// Property: sock_address(data, len) never crashes on arbitrary input.
/// is_v4()/is_v6()/family() must be mutually consistent; copy must be equal.

#include <cstdint>
#include <cstring>
#include <socketpp/net/address.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    // Clamp length to sock_address capacity (128 bytes)
    auto len = static_cast<uint32_t>(size > 128 ? 128 : size);

    socketpp::sock_address sa(data, len);

    // Accessors must not crash
    bool v4 = sa.is_v4();
    bool v6 = sa.is_v6();
    auto fam = sa.family();
    (void)fam;

    // Cannot be both v4 and v6
    if (v4 && v6)
        __builtin_trap();

    // Size must match what we provided
    if (sa.size() != len)
        __builtin_trap();

    // Copy must be equal
    socketpp::sock_address copy(sa);

    if (!(copy == sa))
        __builtin_trap();

    if (copy != sa)
        __builtin_trap();

    if (copy.size() != sa.size())
        __builtin_trap();

    // Verify raw data round-trip
    if (std::memcmp(copy.data(), sa.data(), sa.size()) != 0)
        __builtin_trap();

    return 0;
}

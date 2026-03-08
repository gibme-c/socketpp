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

#include "../platform/detect_internal.hpp"

#include <socketpp/net/address.hpp>

namespace socketpp
{

    sock_address::sock_address() noexcept: len_(sizeof(storage_))
    {
        std::memset(storage_, 0, sizeof(storage_));
    }

    sock_address::sock_address(const void *addr, uint32_t len) noexcept: len_(len)
    {
        std::memset(storage_, 0, sizeof(storage_));

        if (addr && len > 0)
        {
            const auto copy_len =
                static_cast<size_t>(len) < sizeof(storage_) ? static_cast<size_t>(len) : sizeof(storage_);

            std::memcpy(storage_, addr, copy_len);
        }
    }

    bool sock_address::is_v4() const noexcept
    {
        return reinterpret_cast<const sockaddr *>(storage_)->sa_family == AF_INET;
    }

    bool sock_address::is_v6() const noexcept
    {
        return reinterpret_cast<const sockaddr *>(storage_)->sa_family == AF_INET6;
    }

    address_family sock_address::family() const noexcept
    {
        const auto fam = reinterpret_cast<const sockaddr *>(storage_)->sa_family;

        return fam == AF_INET6 ? address_family::ipv6 : address_family::ipv4;
    }

} // namespace socketpp

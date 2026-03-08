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

#ifndef SOCKETPP_NET_INET6_HPP
#define SOCKETPP_NET_INET6_HPP

#include <cstdint>
#include <functional>
#include <ostream>
#include <socketpp/net/address.hpp>
#include <socketpp/platform/error.hpp>
#include <string>
#include <string_view>

namespace socketpp
{

    class inet4_address;

    class inet6_address
    {
      public:
        inet6_address() noexcept;

        inet6_address(const uint8_t (&addr)[16], uint16_t port, uint32_t scope_id = 0, uint32_t flowinfo = 0) noexcept;

        static result<inet6_address> parse(std::string_view ip, uint16_t port, uint32_t scope_id = 0) noexcept;

        static inet6_address any(uint16_t port) noexcept;

        static inet6_address loopback(uint16_t port) noexcept;

        uint16_t port() const noexcept;

        uint32_t scope_id() const noexcept;

        uint32_t flowinfo() const noexcept;

        void bytes(uint8_t (&out)[16]) const noexcept;

        std::string to_string() const;

        bool is_v4_mapped() const noexcept;

        result<inet4_address> to_v4() const noexcept;

        bool is_link_local() const noexcept;

        operator sock_address() const noexcept;

        std::size_t hash_value() const noexcept;

        // ── Comparison Operators ─────────────────────────────────────────────

        friend bool operator==(const inet6_address &lhs, const inet6_address &rhs) noexcept;

        friend bool operator!=(const inet6_address &lhs, const inet6_address &rhs) noexcept
        {
            return !(lhs == rhs);
        }

        friend bool operator<(const inet6_address &lhs, const inet6_address &rhs) noexcept;

        // ── Stream Output ────────────────────────────────────────────────────

        friend std::ostream &operator<<(std::ostream &os, const inet6_address &addr)
        {
            return os << addr.to_string();
        }

      private:
        alignas(4) unsigned char storage_[28];
    };

} // namespace socketpp

// ── std::hash specialization ─────────────────────────────────────────────────

template<> struct std::hash<socketpp::inet6_address>
{
    std::size_t operator()(const socketpp::inet6_address &addr) const noexcept
    {
        return addr.hash_value();
    }
};

#endif // SOCKETPP_NET_INET6_HPP

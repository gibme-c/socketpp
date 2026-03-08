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

#ifndef SOCKETPP_NET_INET4_HPP
#define SOCKETPP_NET_INET4_HPP

#include <cstdint>
#include <functional>
#include <ostream>
#include <socketpp/net/address.hpp>
#include <socketpp/platform/error.hpp>
#include <string>
#include <string_view>

namespace socketpp
{

    class inet4_address
    {
      public:
        inet4_address() noexcept;

        inet4_address(uint32_t address, uint16_t port) noexcept;

        inet4_address(std::string_view ip, uint16_t port) noexcept;

        static result<inet4_address> parse(std::string_view ip, uint16_t port) noexcept;

        static inet4_address any(uint16_t port) noexcept;

        static inet4_address loopback(uint16_t port) noexcept;

        uint32_t ip() const noexcept;

        uint16_t port() const noexcept;

        std::string to_string() const;

        operator sock_address() const noexcept;

        std::size_t hash_value() const noexcept;

        // ── Comparison Operators ─────────────────────────────────────────────

        friend bool operator==(const inet4_address &lhs, const inet4_address &rhs) noexcept
        {
            return lhs.ip() == rhs.ip() && lhs.port() == rhs.port();
        }

        friend bool operator!=(const inet4_address &lhs, const inet4_address &rhs) noexcept
        {
            return !(lhs == rhs);
        }

        friend bool operator<(const inet4_address &lhs, const inet4_address &rhs) noexcept
        {
            if (lhs.ip() != rhs.ip())
                return lhs.ip() < rhs.ip();

            return lhs.port() < rhs.port();
        }

        // ── Stream Output ────────────────────────────────────────────────────

        friend std::ostream &operator<<(std::ostream &os, const inet4_address &addr)
        {
            return os << addr.to_string();
        }

      private:
        alignas(4) unsigned char storage_[16];
    };

} // namespace socketpp

// ── std::hash specialization ─────────────────────────────────────────────────

template<> struct std::hash<socketpp::inet4_address>
{
    std::size_t operator()(const socketpp::inet4_address &addr) const noexcept
    {
        return addr.hash_value();
    }
};

#endif // SOCKETPP_NET_INET4_HPP

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

#include <cstring>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>

namespace socketpp
{

    namespace
    {
        sockaddr_in6 &as_sin6(unsigned char *storage) noexcept
        {
            return *reinterpret_cast<sockaddr_in6 *>(storage);
        }

        const sockaddr_in6 &as_sin6(const unsigned char *storage) noexcept
        {
            return *reinterpret_cast<const sockaddr_in6 *>(storage);
        }
    } // namespace

    inet6_address::inet6_address() noexcept
    {
        static_assert(sizeof(storage_) >= sizeof(sockaddr_in6), "inet6 opaque storage too small");

        std::memset(storage_, 0, sizeof(storage_));

        as_sin6(storage_).sin6_family = AF_INET6;
    }

    inet6_address::inet6_address(
        const uint8_t (&addr)[16],
        uint16_t port,
        uint32_t scope_id,
        uint32_t flowinfo) noexcept
    {
        std::memset(storage_, 0, sizeof(storage_));

        auto &sin6 = as_sin6(storage_);

        sin6.sin6_family = AF_INET6;
        std::memcpy(&sin6.sin6_addr, addr, 16);
        sin6.sin6_port = htons(port);
        sin6.sin6_scope_id = scope_id;
        sin6.sin6_flowinfo = flowinfo;
    }

    result<inet6_address> inet6_address::parse(std::string_view ip, uint16_t port, uint32_t scope_id) noexcept
    {
        inet6_address result_addr;

        auto &sin6 = as_sin6(result_addr.storage_);

        sin6.sin6_port = htons(port);
        sin6.sin6_scope_id = scope_id;

        char buf[INET6_ADDRSTRLEN] = {};
        const auto copy_len = ip.size() < sizeof(buf) - 1 ? ip.size() : sizeof(buf) - 1;

        std::memcpy(buf, ip.data(), copy_len);

        const int rc = inet_pton(AF_INET6, buf, &sin6.sin6_addr);

        if (rc != 1)
            return std::make_error_code(std::errc::invalid_argument);

        if (result_addr.is_link_local() && scope_id == 0)
            return make_error_code(errc::missing_scope_id);

        return result_addr;
    }

    inet6_address inet6_address::any(uint16_t port) noexcept
    {
        uint8_t zeros[16] = {};

        return inet6_address(zeros, port);
    }

    inet6_address inet6_address::loopback(uint16_t port) noexcept
    {
        uint8_t addr[16] = {};
        addr[15] = 1;

        return inet6_address(addr, port);
    }

    uint16_t inet6_address::port() const noexcept
    {
        return ntohs(as_sin6(storage_).sin6_port);
    }

    uint32_t inet6_address::scope_id() const noexcept
    {
        return as_sin6(storage_).sin6_scope_id;
    }

    uint32_t inet6_address::flowinfo() const noexcept
    {
        return as_sin6(storage_).sin6_flowinfo;
    }

    void inet6_address::bytes(uint8_t (&out)[16]) const noexcept
    {
        std::memcpy(out, as_sin6(storage_).sin6_addr.s6_addr, 16);
    }

    std::string inet6_address::to_string() const
    {
        const auto &sin6 = as_sin6(storage_);

        char buf[INET6_ADDRSTRLEN] = {};

        inet_ntop(AF_INET6, &sin6.sin6_addr, buf, sizeof(buf));

        std::string s("[");
        s += buf;

        if (sin6.sin6_scope_id != 0)
        {
            s += '%';
            s += std::to_string(sin6.sin6_scope_id);
        }

        s += "]:";
        s += std::to_string(ntohs(sin6.sin6_port));

        return s;
    }

    bool inet6_address::is_v4_mapped() const noexcept
    {
        const auto *b = as_sin6(storage_).sin6_addr.s6_addr;

        for (int i = 0; i < 10; ++i)
        {
            if (b[i] != 0)
                return false;
        }

        return b[10] == 0xff && b[11] == 0xff;
    }

    result<inet4_address> inet6_address::to_v4() const noexcept
    {
        if (!is_v4_mapped())
            return make_error_code(errc::invalid_state);

        const auto *b = as_sin6(storage_).sin6_addr.s6_addr;

        uint32_t ip_net = 0;

        std::memcpy(&ip_net, b + 12, 4);

        return inet4_address(ntohl(ip_net), port());
    }

    bool inet6_address::is_link_local() const noexcept
    {
        const auto *b = as_sin6(storage_).sin6_addr.s6_addr;

        return b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
    }

    inet6_address::operator sock_address() const noexcept
    {
        return sock_address(storage_, sizeof(sockaddr_in6));
    }

    std::size_t inet6_address::hash_value() const noexcept
    {
        const auto *b = as_sin6(storage_).sin6_addr.s6_addr;

        std::size_t h = 0;

        for (int i = 0; i < 16; ++i)
            h = h * 131 + b[i];

        h ^= std::hash<uint16_t> {}(port()) << 1;
        h ^= std::hash<uint32_t> {}(scope_id()) << 2;

        return h;
    }

    bool operator==(const inet6_address &lhs, const inet6_address &rhs) noexcept
    {
        const auto &l = as_sin6(lhs.storage_);
        const auto &r = as_sin6(rhs.storage_);

        return std::memcmp(&l.sin6_addr, &r.sin6_addr, sizeof(in6_addr)) == 0 && l.sin6_port == r.sin6_port
               && l.sin6_scope_id == r.sin6_scope_id;
    }

    bool operator<(const inet6_address &lhs, const inet6_address &rhs) noexcept
    {
        const auto &l = as_sin6(lhs.storage_);
        const auto &r = as_sin6(rhs.storage_);

        const int cmp = std::memcmp(&l.sin6_addr, &r.sin6_addr, sizeof(in6_addr));

        if (cmp != 0)
            return cmp < 0;

        if (l.sin6_port != r.sin6_port)
            return ntohs(l.sin6_port) < ntohs(r.sin6_port);

        return l.sin6_scope_id < r.sin6_scope_id;
    }

} // namespace socketpp

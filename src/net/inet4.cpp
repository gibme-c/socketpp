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

namespace socketpp
{

    namespace
    {
        sockaddr_in &as_sin(unsigned char *storage) noexcept
        {
            return *reinterpret_cast<sockaddr_in *>(storage);
        }

        const sockaddr_in &as_sin(const unsigned char *storage) noexcept
        {
            return *reinterpret_cast<const sockaddr_in *>(storage);
        }
    } // namespace

    inet4_address::inet4_address() noexcept
    {
        static_assert(sizeof(storage_) >= sizeof(sockaddr_in), "inet4 opaque storage too small");

        std::memset(storage_, 0, sizeof(storage_));

        as_sin(storage_).sin_family = AF_INET;
    }

    inet4_address::inet4_address(uint32_t address, uint16_t port) noexcept
    {
        std::memset(storage_, 0, sizeof(storage_));

        auto &sin = as_sin(storage_);

        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(address);
        sin.sin_port = htons(port);
    }

    inet4_address::inet4_address(std::string_view ip, uint16_t port) noexcept
    {
        std::memset(storage_, 0, sizeof(storage_));

        auto &sin = as_sin(storage_);

        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);

        char buf[INET_ADDRSTRLEN] = {};
        const auto copy_len = ip.size() < sizeof(buf) - 1 ? ip.size() : sizeof(buf) - 1;

        std::memcpy(buf, ip.data(), copy_len);

        inet_pton(AF_INET, buf, &sin.sin_addr);
    }

    result<inet4_address> inet4_address::parse(std::string_view ip, uint16_t port) noexcept
    {
        inet4_address result_addr;

        auto &sin = as_sin(result_addr.storage_);

        sin.sin_port = htons(port);

        char buf[INET_ADDRSTRLEN] = {};
        const auto copy_len = ip.size() < sizeof(buf) - 1 ? ip.size() : sizeof(buf) - 1;

        std::memcpy(buf, ip.data(), copy_len);

        const int rc = inet_pton(AF_INET, buf, &sin.sin_addr);

        if (rc != 1)
            return std::make_error_code(std::errc::invalid_argument);

        return result_addr;
    }

    inet4_address inet4_address::any(uint16_t port) noexcept
    {
        return inet4_address(INADDR_ANY, port);
    }

    inet4_address inet4_address::loopback(uint16_t port) noexcept
    {
        return inet4_address(INADDR_LOOPBACK, port);
    }

    uint32_t inet4_address::ip() const noexcept
    {
        return ntohl(as_sin(storage_).sin_addr.s_addr);
    }

    uint16_t inet4_address::port() const noexcept
    {
        return ntohs(as_sin(storage_).sin_port);
    }

    std::string inet4_address::to_string() const
    {
        const auto &sin = as_sin(storage_);

        char buf[INET_ADDRSTRLEN] = {};

        inet_ntop(AF_INET, &sin.sin_addr, buf, sizeof(buf));

        std::string s(buf);
        s += ':';
        s += std::to_string(ntohs(sin.sin_port));

        return s;
    }

    inet4_address::operator sock_address() const noexcept
    {
        return sock_address(storage_, sizeof(sockaddr_in));
    }

    std::size_t inet4_address::hash_value() const noexcept
    {
        const auto h1 = std::hash<uint32_t> {}(ip());
        const auto h2 = std::hash<uint16_t> {}(port());

        return h1 ^ (h2 << 1);
    }

} // namespace socketpp

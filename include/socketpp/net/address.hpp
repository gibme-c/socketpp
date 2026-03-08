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

#ifndef SOCKETPP_NET_ADDRESS_HPP
#define SOCKETPP_NET_ADDRESS_HPP

#include <cstdint>
#include <cstring>
#include <socketpp/platform/types.hpp>

namespace socketpp
{

    struct sock_address
    {
        sock_address() noexcept;

        sock_address(const void *addr, uint32_t len) noexcept;

        bool is_v4() const noexcept;

        bool is_v6() const noexcept;

        address_family family() const noexcept;

        void *data() noexcept
        {
            return storage_;
        }

        const void *data() const noexcept
        {
            return storage_;
        }

        uint32_t size() const noexcept
        {
            return len_;
        }

        void set_size(uint32_t len) noexcept
        {
            len_ = len;
        }

        uint32_t capacity() const noexcept
        {
            return sizeof(storage_);
        }

        // ── Comparison Operators ─────────────────────────────────────────────

        friend bool operator==(const sock_address &lhs, const sock_address &rhs) noexcept
        {
            if (lhs.len_ != rhs.len_)
                return false;

            return std::memcmp(lhs.storage_, rhs.storage_, lhs.len_) == 0;
        }

        friend bool operator!=(const sock_address &lhs, const sock_address &rhs) noexcept
        {
            return !(lhs == rhs);
        }

        friend bool operator<(const sock_address &lhs, const sock_address &rhs) noexcept
        {
            if (lhs.len_ != rhs.len_)
                return lhs.len_ < rhs.len_;

            return std::memcmp(lhs.storage_, rhs.storage_, lhs.len_) < 0;
        }

      private:
        alignas(8) unsigned char storage_[128];
        uint32_t len_;
    };

} // namespace socketpp

#endif // SOCKETPP_NET_ADDRESS_HPP

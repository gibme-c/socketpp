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

#ifndef SOCKETPP_PLATFORM_TYPES_HPP
#define SOCKETPP_PLATFORM_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace socketpp
{

    // ── Socket Handle Type ───────────────────────────────────────────────────────
    // intptr_t holds both POSIX int and Windows UINT_PTR (SOCKET).

    using socket_t = std::intptr_t;
    inline constexpr socket_t invalid_socket = -1;

    // ── Portable Enums ───────────────────────────────────────────────────────────

    enum class address_family : uint8_t
    {
        ipv4,
        ipv6
    };
    enum class socket_type : uint8_t
    {
        stream,
        dgram
    };
    enum class shutdown_mode : uint8_t
    {
        read,
        write,
        both
    };

    // ── Default Backlog ──────────────────────────────────────────────────────────

    inline constexpr int default_backlog = 128;

    // ── iovec ────────────────────────────────────────────────────────────────────
    // Unconditional portable definition. Conversion to WSABUF / POSIX iovec
    // happens in .cpp files only.

    struct iovec
    {
        void *iov_base;
        size_t iov_len;
    };

    // ── Minimal span<T> shim (C++17) ────────────────────────────────────────────

    template<typename T> class span
    {
      public:
        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using pointer = T *;
        using const_pointer = const T *;
        using reference = T &;
        using iterator = pointer;
        using const_iterator = const_pointer;

        constexpr span() noexcept: data_(nullptr), size_(0) {}

        constexpr span(pointer ptr, size_type count) noexcept: data_(ptr), size_(count) {}

        constexpr span(pointer first, pointer last) noexcept: data_(first), size_(static_cast<size_type>(last - first))
        {
        }

        template<std::size_t N> constexpr span(T (&arr)[N]) noexcept: data_(arr), size_(N) {}

        template<std::size_t N> constexpr span(std::array<value_type, N> &arr) noexcept: data_(arr.data()), size_(N) {}

        template<std::size_t N, typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
        constexpr span(const std::array<value_type, N> &arr) noexcept: data_(arr.data()), size_(N)
        {
        }

        span(std::vector<value_type> &vec) noexcept: data_(vec.data()), size_(vec.size()) {}

        template<typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
        span(const std::vector<value_type> &vec) noexcept: data_(vec.data()), size_(vec.size())
        {
        }

        constexpr pointer data() const noexcept
        {
            return data_;
        }
        constexpr size_type size() const noexcept
        {
            return size_;
        }
        constexpr bool empty() const noexcept
        {
            return size_ == 0;
        }

        constexpr reference operator[](size_type idx) const noexcept
        {
            return data_[idx];
        }

        constexpr iterator begin() const noexcept
        {
            return data_;
        }
        constexpr iterator end() const noexcept
        {
            return data_ + size_;
        }

        constexpr span subspan(size_type offset, size_type count = static_cast<size_type>(-1)) const noexcept
        {
            if (offset >= size_)
                return {};
            const auto remaining = size_ - offset;
            return {data_ + offset, count < remaining ? count : remaining};
        }

      private:
        pointer data_;
        size_type size_;
    };

} // namespace socketpp

#endif // SOCKETPP_PLATFORM_TYPES_HPP

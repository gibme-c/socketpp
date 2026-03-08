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

#ifndef SOCKETPP_PLATFORM_ERROR_HPP
#define SOCKETPP_PLATFORM_ERROR_HPP

#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace socketpp
{

    // ── Portable Error Codes ─────────────────────────────────────────────────────

    enum class errc : int
    {
        success = 0,
        would_block,
        in_progress,
        connection_reset,
        connection_refused,
        connection_aborted,
        not_connected,
        address_in_use,
        network_unreachable,
        timed_out,
        option_not_supported,
        already_bound,
        peer_not_found,
        invalid_state,
        interrupted,
        fd_limit_reached,
        missing_scope_id,
        invalid_argument
    };

    // ── Error Category ───────────────────────────────────────────────────────────

    class error_category_impl : public std::error_category
    {
      public:
        const char *name() const noexcept override
        {
            return "socketpp";
        }

        std::string message(int ev) const override
        {
            switch (static_cast<errc>(ev))
            {
                case errc::success:
                    return "success";
                case errc::would_block:
                    return "operation would block";
                case errc::in_progress:
                    return "operation in progress";
                case errc::connection_reset:
                    return "connection reset by peer";
                case errc::connection_refused:
                    return "connection refused";
                case errc::connection_aborted:
                    return "connection aborted";
                case errc::not_connected:
                    return "socket not connected";
                case errc::address_in_use:
                    return "address already in use";
                case errc::network_unreachable:
                    return "network unreachable";
                case errc::timed_out:
                    return "operation timed out";
                case errc::option_not_supported:
                    return "option not supported on this platform";
                case errc::already_bound:
                    return "socket already bound";
                case errc::peer_not_found:
                    return "peer not found";
                case errc::invalid_state:
                    return "invalid socket state";
                case errc::interrupted:
                    return "operation interrupted";
                case errc::fd_limit_reached:
                    return "file descriptor limit reached";
                case errc::missing_scope_id:
                    return "link-local address requires a scope id";
                case errc::invalid_argument:
                    return "invalid argument";
                default:
                    return "unknown socketpp error";
            }
        }
    };

    inline const std::error_category &error_category() noexcept
    {
        static const error_category_impl instance;
        return instance;
    }

    // ── make_error_code ──────────────────────────────────────────────────────────

    inline std::error_code make_error_code(errc e) noexcept
    {
        return {static_cast<int>(e), error_category()};
    }

} // namespace socketpp

// ── Register errc as an error_code enum ──────────────────────────────────────

template<> struct std::is_error_code_enum<socketpp::errc> : std::true_type
{
};

namespace socketpp
{

    // ── Platform Error Helpers (implemented in error.cpp) ────────────────────────

    std::error_code last_socket_error() noexcept;

    std::error_code normalize_error(std::error_code ec) noexcept;

    // ── result<T> ────────────────────────────────────────────────────────────────

    template<typename T> class result
    {
        static_assert(!std::is_same_v<T, void>, "Use result<void> specialization");

      public:
        result(const T &val) noexcept(std::is_nothrow_copy_constructible_v<T>): has_value_(true)
        {
            new (&storage_) T(val);
        }

        result(T &&val) noexcept(std::is_nothrow_move_constructible_v<T>): has_value_(true)
        {
            new (&storage_) T(std::move(val));
        }

        result(std::error_code ec) noexcept: error_(ec), has_value_(false) {}

        result(errc e) noexcept: error_(make_error_code(e)), has_value_(false) {}

        result(const result &other) noexcept(std::is_nothrow_copy_constructible_v<T>):
            error_(other.error_), has_value_(other.has_value_)
        {
            if (has_value_)
                new (&storage_) T(other.value_ref());
        }

        result &operator=(const result &other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        {
            if (this != &other)
            {
                destroy();
                has_value_ = other.has_value_;
                error_ = other.error_;
                if (has_value_)
                    new (&storage_) T(other.value_ref());
            }
            return *this;
        }

        result(result &&other) noexcept(std::is_nothrow_move_constructible_v<T>):
            error_(other.error_), has_value_(other.has_value_)
        {
            if (has_value_)
                new (&storage_) T(std::move(other.value_ref()));
        }

        result &operator=(result &&other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (this != &other)
            {
                destroy();
                has_value_ = other.has_value_;
                error_ = other.error_;
                if (has_value_)
                    new (&storage_) T(std::move(other.value_ref()));
            }
            return *this;
        }

        ~result()
        {
            destroy();
        }

        explicit operator bool() const noexcept
        {
            return has_value_;
        }

        T &value() &noexcept
        {
            assert_has_value();
            return value_ref();
        }

        const T &value() const &noexcept
        {
            assert_has_value();
            return value_ref();
        }

        T &&value() &&noexcept
        {
            assert_has_value();
            return std::move(value_ref());
        }

        std::error_code error() const noexcept
        {
            return has_value_ ? std::error_code {} : error_;
        }

        std::string message() const
        {
            return error().message();
        }

      private:
        T &value_ref() noexcept
        {
            return *reinterpret_cast<T *>(&storage_);
        }

        const T &value_ref() const noexcept
        {
            return *reinterpret_cast<const T *>(&storage_);
        }

        void assert_has_value() const noexcept
        {
            if (!has_value_)
                std::abort();
        }

        void destroy() noexcept
        {
            if (has_value_)
                value_ref().~T();
        }

        alignas(T) std::byte storage_[sizeof(T)];
        std::error_code error_;
        bool has_value_;
    };

    // ── result<void> specialization ──────────────────────────────────────────────

    template<> class result<void>
    {
      public:
        result() noexcept: error_() {}

        result(std::error_code ec) noexcept: error_(ec) {}

        result(errc e) noexcept: error_(make_error_code(e)) {}

        explicit operator bool() const noexcept
        {
            return !error_;
        }

        std::error_code error() const noexcept
        {
            return error_;
        }

        std::string message() const
        {
            return error_.message();
        }

      private:
        std::error_code error_;
    };

} // namespace socketpp

#endif // SOCKETPP_PLATFORM_ERROR_HPP

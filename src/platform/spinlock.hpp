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

#ifndef SOCKETPP_SRC_PLATFORM_SPINLOCK_HPP
#define SOCKETPP_SRC_PLATFORM_SPINLOCK_HPP

#include <atomic>

#if defined(_MSC_VER)
#include <immintrin.h>
#endif

namespace socketpp::detail
{

    inline void cpu_pause() noexcept
    {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    class spinlock
    {
      public:
        constexpr spinlock() noexcept = default;

        spinlock(const spinlock &) = delete;
        spinlock &operator=(const spinlock &) = delete;

        void lock() noexcept
        {
            while (flag_.test_and_set(std::memory_order_acquire))
            {
                cpu_pause();
            }
        }

        void unlock() noexcept
        {
            flag_.clear(std::memory_order_release);
        }

      private:
        std::atomic_flag flag_ = {};
    };

    template<typename Lockable> class scoped_lock
    {
      public:
        explicit scoped_lock(Lockable &m) noexcept: m_(m)
        {
            m_.lock();
        }
        ~scoped_lock() noexcept
        {
            m_.unlock();
        }

        scoped_lock(const scoped_lock &) = delete;
        scoped_lock &operator=(const scoped_lock &) = delete;

      private:
        Lockable &m_;
    };

} // namespace socketpp::detail

#endif // SOCKETPP_SRC_PLATFORM_SPINLOCK_HPP

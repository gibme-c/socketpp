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

/// @file test_harness.hpp
/// Shared test harness macros and helpers for socketpp test suites.

#ifndef SOCKETPP_TEST_HARNESS_HPP
#define SOCKETPP_TEST_HARNESS_HPP

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

static int g_fail_count = 0;
static int g_test_count = 0;

#define CHECK(expr)                                                                           \
    do                                                                                        \
    {                                                                                         \
        ++g_test_count;                                                                       \
        if (!(expr))                                                                          \
        {                                                                                     \
            std::cerr << "  FAIL: " << #expr << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_fail_count;                                                                   \
        }                                                                                     \
    } while (0)

#define CHECK_MSG(expr, msg)                                                                                  \
    do                                                                                                        \
    {                                                                                                         \
        ++g_test_count;                                                                                       \
        if (!(expr))                                                                                          \
        {                                                                                                     \
            std::cerr << "  FAIL: " << #expr << " - " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_fail_count;                                                                                   \
        }                                                                                                     \
    } while (0)

#define RUN_TEST(fn)                        \
    do                                      \
    {                                       \
        int fail_before = g_fail_count;     \
        std::cerr << "  " << #fn << "... "; \
        fn();                               \
        if (g_fail_count == fail_before)    \
            std::cerr << "ok\n";            \
        else                                \
            std::cerr << "FAILED\n";        \
    } while (0)

namespace socketpp_test
{

    inline void platform_sleep_ms(int ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    inline bool wait_for(std::atomic<bool> &flag, int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (!flag.load(std::memory_order_relaxed))
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return true;
    }

    inline bool wait_for_count(std::atomic<int> &counter, int target, int timeout_ms = 5000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (counter.load(std::memory_order_relaxed) < target)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return true;
    }

} // namespace socketpp_test

#endif // SOCKETPP_TEST_HARNESS_HPP

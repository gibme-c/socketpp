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

#ifndef SOCKETPP_PLATFORM_DETECT_HPP
#define SOCKETPP_PLATFORM_DETECT_HPP

// ── OS Detection ─────────────────────────────────────────────────────────────

#if defined(_WIN32) || defined(_WIN64)
#define SOCKETPP_OS_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define SOCKETPP_OS_MACOS 1
#elif defined(__linux__)
#define SOCKETPP_OS_LINUX 1
#else
#error "socketpp: unsupported platform"
#endif

// ── Compiler Detection ───────────────────────────────────────────────────────

#if defined(_MSC_VER) && !defined(__clang__)
#define SOCKETPP_COMPILER_MSVC 1
#elif defined(__clang__)
#define SOCKETPP_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define SOCKETPP_COMPILER_GCC 1
#else
#error "socketpp: unsupported compiler"
#endif

// ── Force Inline ─────────────────────────────────────────────────────────────

#if defined(SOCKETPP_COMPILER_MSVC)
#define SOCKETPP_FORCEINLINE __forceinline
#else
#define SOCKETPP_FORCEINLINE __attribute__((always_inline)) inline
#endif

// ── Branch Hints ─────────────────────────────────────────────────────────────

#if defined(SOCKETPP_COMPILER_MSVC)
#define SOCKETPP_LIKELY(x) (x)
#define SOCKETPP_UNLIKELY(x) (x)
#else
#define SOCKETPP_LIKELY(x) __builtin_expect(!!(x), 1)
#define SOCKETPP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#endif // SOCKETPP_PLATFORM_DETECT_HPP

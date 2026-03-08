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

/// @file loop.cpp
/// @brief event_loop constructor and destructor implementation.
///
/// The constructor creates the platform-appropriate dispatcher and initializes
/// the double-buffer post queue. The static_assert below ensures a compile-time
/// error on unsupported platforms rather than a confusing linker failure.

#include <socketpp/event/loop.hpp>
#include <socketpp/platform/detect.hpp>

#if !defined(SOCKETPP_OS_LINUX) && !defined(SOCKETPP_OS_MACOS) && !defined(SOCKETPP_OS_WINDOWS)
static_assert(false, "socketpp: no dispatcher backend for this platform (need Linux, macOS, or Windows)");
#endif

namespace socketpp
{

    // Initializes the swap-buffer queue: queue_a_ is the initial active (producer) buffer,
    // queue_b_ is the initial drain (consumer) buffer. dispatcher::create() selects the
    // platform backend at runtime.
    event_loop::event_loop(): dispatcher_(dispatcher::create()), active_queue_(&queue_a_), drain_queue_(&queue_b_) {}

    event_loop::~event_loop() = default;

} // namespace socketpp

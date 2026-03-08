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

#ifndef SOCKETPP_SOCKET_BUF_PROFILE_HPP
#define SOCKETPP_SOCKET_BUF_PROFILE_HPP

namespace socketpp
{

    /**
     * Describes send and receive buffer sizes for a socket.
     * Provides named presets for common network topologies and a
     * bandwidth-delay product calculator for tuning to specific links.
     */
    struct buf_profile
    {
        int send_size;
        int recv_size;

        /// 256 KB -- suitable for loopback / localhost communication.
        static constexpr buf_profile localhost() noexcept
        {
            return {262144, 262144};
        }

        /// 512 KB -- suitable for local-area networks.
        static constexpr buf_profile lan() noexcept
        {
            return {524288, 524288};
        }

        /// 1 MB -- suitable for wide-area networks.
        static constexpr buf_profile wan() noexcept
        {
            return {1048576, 1048576};
        }

        /// 4 MB -- suitable for high bandwidth-delay product links.
        static constexpr buf_profile high_bdp() noexcept
        {
            return {4194304, 4194304};
        }

        /**
         * Calculate an optimal buffer size from the bandwidth-delay product.
         *
         * @param bw_bytes_sec link bandwidth in bytes per second
         * @param rtt_sec      round-trip time in seconds
         * @return buf_profile  clamped to [4 KB, 16 MB]
         */
        static buf_profile from_bdp(double bw_bytes_sec, double rtt_sec) noexcept
        {
            auto size = static_cast<int>(bw_bytes_sec * rtt_sec);

            if (size < 4096)
                size = 4096;

            if (size > 16777216) // 16 MB cap
                size = 16777216;

            return {size, size};
        }
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_BUF_PROFILE_HPP

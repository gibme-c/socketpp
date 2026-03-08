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

/**
 * @file dgram_peer.cpp
 * @brief Implementation of dgram4_peer and dgram6_peer public methods.
 *
 * All methods forward to the internal dgram_peer_impl template instantiation.
 */

#include "dgram_peer_impl.hpp"

#include <socketpp/dgram.hpp>

namespace socketpp
{

    // impl struct definitions are in dgram_peer_impl.hpp (shared with dgram.cpp).

    // ── dgram4_peer ──────────────────────────────────────────────────────────────

    dgram4_peer::dgram4_peer() = default;

    dgram4_peer::~dgram4_peer()
    {
        if (impl_ && impl_->open_.load(std::memory_order_relaxed))
            relinquish();
    }

    dgram4_peer::dgram4_peer(dgram4_peer &&) noexcept = default;
    dgram4_peer &dgram4_peer::operator=(dgram4_peer &&other) noexcept
    {
        if (this != &other)
        {
            if (impl_ && impl_->open_.load(std::memory_order_relaxed))
                relinquish();

            impl_ = std::move(other.impl_);
        }

        return *this;
    }

    dgram4_peer &dgram4_peer::on_data(data_handler handler)
    {
        impl_->on_data_cb = std::move(handler);
        return *this;
    }

    dgram4_peer &dgram4_peer::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    bool dgram4_peer::send(const void *data, size_t len)
    {
        return impl_->do_send(data, len);
    }

    result<int> dgram4_peer::send_batch(span<const dgram4_send_entry> msgs)
    {
        return impl_->do_send_batch(msgs);
    }

    void dgram4_peer::relinquish()
    {
        if (!impl_)
            return;

        impl_->do_relinquish();
    }

    inet4_address dgram4_peer::peer_addr() const
    {
        return impl_ ? impl_->peer_addr_ : inet4_address {};
    }

    bool dgram4_peer::is_open() const noexcept
    {
        return impl_ && impl_->open_.load(std::memory_order_relaxed);
    }

    timer_handle dgram4_peer::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle dgram4_peer::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void dgram4_peer::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

    // ── dgram6_peer ──────────────────────────────────────────────────────────────

    dgram6_peer::dgram6_peer() = default;

    dgram6_peer::~dgram6_peer()
    {
        if (impl_ && impl_->open_.load(std::memory_order_relaxed))
            relinquish();
    }

    dgram6_peer::dgram6_peer(dgram6_peer &&) noexcept = default;
    dgram6_peer &dgram6_peer::operator=(dgram6_peer &&other) noexcept
    {
        if (this != &other)
        {
            if (impl_ && impl_->open_.load(std::memory_order_relaxed))
                relinquish();

            impl_ = std::move(other.impl_);
        }

        return *this;
    }

    dgram6_peer &dgram6_peer::on_data(data_handler handler)
    {
        impl_->on_data_cb = std::move(handler);
        return *this;
    }

    dgram6_peer &dgram6_peer::on_error(error_handler handler)
    {
        impl_->on_error_cb = std::move(handler);
        return *this;
    }

    bool dgram6_peer::send(const void *data, size_t len)
    {
        return impl_->do_send(data, len);
    }

    result<int> dgram6_peer::send_batch(span<const dgram6_send_entry> msgs)
    {
        return impl_->do_send_batch(msgs);
    }

    void dgram6_peer::relinquish()
    {
        if (!impl_)
            return;

        impl_->do_relinquish();
    }

    inet6_address dgram6_peer::peer_addr() const
    {
        return impl_ ? impl_->peer_addr_ : inet6_address {};
    }

    bool dgram6_peer::is_open() const noexcept
    {
        return impl_ && impl_->open_.load(std::memory_order_relaxed);
    }

    timer_handle dgram6_peer::defer(std::chrono::milliseconds delay, std::function<void()> cb)
    {
        return impl_->do_defer(delay, std::move(cb));
    }

    timer_handle dgram6_peer::repeat(std::chrono::milliseconds interval, std::function<void()> cb)
    {
        return impl_->do_repeat(interval, std::move(cb));
    }

    void dgram6_peer::post(std::function<void()> cb)
    {
        impl_->do_post(std::move(cb));
    }

} // namespace socketpp

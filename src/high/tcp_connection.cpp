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
 * @file tcp_connection.cpp
 * @brief Implementation of tcp4_connection and tcp6_connection public APIs.
 *
 * Each method is a thin delegation to the shared tcp_conn_impl. The destructor
 * has special logic: if this is the last handle (use_count == 1) and the
 * connection is still open, it initiates an asynchronous close. This prevents
 * leaked connections when a server or client drops its wrapper without
 * explicitly closing.
 */

#include "tcp_connection_impl.hpp"

namespace socketpp
{

    // ── tcp4_connection ──────────────────────────────────────────────────────────

    tcp4_connection::tcp4_connection(std::shared_ptr<impl> p): impl_(std::move(p)) {}

    tcp4_connection::~tcp4_connection()
    {
        // Last-handle cleanup: if nobody else holds the impl and the connection
        // is still open, trigger an async close so the socket doesn't leak.
        // The loop_ check guards against moved-from or partially-constructed state.
        if (impl_ && impl_.use_count() == 1 && impl_->loop_ && !impl_->closed_.load(std::memory_order_relaxed))
            impl_->close_from_user();
    }

    tcp4_connection::tcp4_connection(tcp4_connection &&) noexcept = default;
    tcp4_connection &tcp4_connection::operator=(tcp4_connection &&) noexcept = default;

    void tcp4_connection::on_data(data_handler handler)
    {
        impl_->on_data_ = std::move(handler);
    }

    void tcp4_connection::on_close(close_handler handler)
    {
        impl_->on_close_ = std::move(handler);
    }

    void tcp4_connection::on_error(error_handler handler)
    {
        impl_->on_error_ = std::move(handler);
    }

    bool tcp4_connection::send(const void *data, size_t len)
    {
        return impl_->enqueue_send(data, len);
    }

    bool tcp4_connection::send(const std::string &data)
    {
        return impl_->enqueue_send(data.data(), data.size());
    }

    void tcp4_connection::close()
    {
        impl_->close_from_user();
    }

    inet4_address tcp4_connection::peer_addr() const
    {
        return impl_->peer_;
    }

    inet4_address tcp4_connection::local_addr() const
    {
        return impl_->local_;
    }

    bool tcp4_connection::is_open() const noexcept
    {
        return !impl_->closed_.load(std::memory_order_relaxed);
    }

    size_t tcp4_connection::write_queue_bytes() const noexcept
    {
        // Lock required: write_queue_bytes_ is modified by enqueue_send() (any thread)
        // and drain_write_queue() (event loop thread).
        std::lock_guard<std::mutex> lock(impl_->write_mutex_);
        return impl_->write_queue_bytes_;
    }

    // ── tcp6_connection ──────────────────────────────────────────────────────────

    tcp6_connection::tcp6_connection(std::shared_ptr<impl> p): impl_(std::move(p)) {}

    tcp6_connection::~tcp6_connection()
    {
        // Same last-handle cleanup as tcp4_connection.
        if (impl_ && impl_.use_count() == 1 && impl_->loop_ && !impl_->closed_.load(std::memory_order_relaxed))
            impl_->close_from_user();
    }

    tcp6_connection::tcp6_connection(tcp6_connection &&) noexcept = default;
    tcp6_connection &tcp6_connection::operator=(tcp6_connection &&) noexcept = default;

    void tcp6_connection::on_data(data_handler handler)
    {
        impl_->on_data_ = std::move(handler);
    }

    void tcp6_connection::on_close(close_handler handler)
    {
        impl_->on_close_ = std::move(handler);
    }

    void tcp6_connection::on_error(error_handler handler)
    {
        impl_->on_error_ = std::move(handler);
    }

    bool tcp6_connection::send(const void *data, size_t len)
    {
        return impl_->enqueue_send(data, len);
    }

    bool tcp6_connection::send(const std::string &data)
    {
        return impl_->enqueue_send(data.data(), data.size());
    }

    void tcp6_connection::close()
    {
        impl_->close_from_user();
    }

    inet6_address tcp6_connection::peer_addr() const
    {
        return impl_->peer_;
    }

    inet6_address tcp6_connection::local_addr() const
    {
        return impl_->local_;
    }

    bool tcp6_connection::is_open() const noexcept
    {
        return !impl_->closed_.load(std::memory_order_relaxed);
    }

    size_t tcp6_connection::write_queue_bytes() const noexcept
    {
        std::lock_guard<std::mutex> lock(impl_->write_mutex_);
        return impl_->write_queue_bytes_;
    }

} // namespace socketpp

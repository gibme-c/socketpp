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

#ifndef SOCKETPP_SOCKET_OPTIONS_HPP
#define SOCKETPP_SOCKET_OPTIONS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <socketpp/net/address.hpp>
#include <socketpp/platform/capabilities.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/platform/types.hpp>
#include <socketpp/socket/buf_profile.hpp>
#include <vector>

namespace socketpp
{

    // ── Portable Socket Option IDs ───────────────────────────────────────────────

    enum class socket_option_id : uint16_t
    {
        reuse_addr,
        reuse_port,
        exclusive_addr,
        recv_buf,
        send_buf,
        tcp_nodelay,
        tcp_cork,
        tcp_notsent_lowat,
        tcp_user_timeout,
        tcp_fastopen,
        tcp_defer_accept,
        keep_alive,
        keep_alive_idle,
        keep_alive_interval,
        keep_alive_count,
        linger_opt,
        ipv6_only,
        ip_tos,
        broadcast,
        multicast_join,
        multicast_leave,
        multicast_ttl,
        multicast_loop,
        multicast_ttl_v6,
        multicast_loop_v6,
        multicast_if,
        multicast_if_v6,
    };

    // ── Small Vector (inline storage for typical option counts) ──────────────────

    namespace detail
    {

        template<typename T, size_t N> class small_vector
        {
          public:
            small_vector() noexcept = default;

            ~small_vector()
            {
                destroy_inline();
            }

            small_vector(const small_vector &o): size_(o.size_), on_heap_(o.on_heap_)
            {
                if (on_heap_)
                {
                    heap_ = o.heap_;
                }
                else
                {
                    for (size_t i = 0; i < size_; ++i)
                        new (slot(i)) T(o.get(i));
                }
            }

            small_vector &operator=(const small_vector &o)
            {
                if (this != &o)
                {
                    clear();
                    size_ = o.size_;
                    on_heap_ = o.on_heap_;

                    if (on_heap_)
                    {
                        heap_ = o.heap_;
                    }
                    else
                    {
                        for (size_t i = 0; i < size_; ++i)
                            new (slot(i)) T(o.get(i));
                    }
                }

                return *this;
            }

            small_vector(small_vector &&o) noexcept: size_(o.size_), on_heap_(o.on_heap_)
            {
                if (on_heap_)
                {
                    heap_ = std::move(o.heap_);
                }
                else
                {
                    for (size_t i = 0; i < size_; ++i)
                    {
                        new (slot(i)) T(std::move(o.ref(i)));
                        o.ref(i).~T();
                    }
                }

                o.size_ = 0;
                o.on_heap_ = false;
            }

            small_vector &operator=(small_vector &&o) noexcept
            {
                if (this != &o)
                {
                    clear();
                    size_ = o.size_;
                    on_heap_ = o.on_heap_;

                    if (on_heap_)
                    {
                        heap_ = std::move(o.heap_);
                    }
                    else
                    {
                        for (size_t i = 0; i < size_; ++i)
                        {
                            new (slot(i)) T(std::move(o.ref(i)));
                            o.ref(i).~T();
                        }
                    }

                    o.size_ = 0;
                    o.on_heap_ = false;
                }

                return *this;
            }

            void push_back(T val)
            {
                if (!on_heap_ && size_ < N)
                {
                    new (slot(size_)) T(std::move(val));
                }
                else
                {
                    if (!on_heap_)
                        spill();

                    heap_.push_back(std::move(val));
                }

                ++size_;
            }

            size_t size() const noexcept
            {
                return size_;
            }

            bool empty() const noexcept
            {
                return size_ == 0;
            }

            const T *begin() const noexcept
            {
                return on_heap_ ? heap_.data() : (size_ ? &get(0) : nullptr);
            }

            const T *end() const noexcept
            {
                return begin() + size_;
            }

          private:
            alignas(T) std::byte storage_[N * sizeof(T)];
            std::vector<T> heap_;
            size_t size_ = 0;
            bool on_heap_ = false;

            void *slot(size_t i) noexcept
            {
                return storage_ + i * sizeof(T);
            }

            T &ref(size_t i) noexcept
            {
                return *reinterpret_cast<T *>(storage_ + i * sizeof(T));
            }

            const T &get(size_t i) const noexcept
            {
                return *reinterpret_cast<const T *>(storage_ + i * sizeof(T));
            }

            void destroy_inline()
            {
                if (!on_heap_)
                {
                    for (size_t i = 0; i < size_; ++i)
                        ref(i).~T();
                }
            }

            void clear()
            {
                if (on_heap_)
                {
                    heap_.clear();
                }
                else
                {
                    destroy_inline();
                }

                size_ = 0;
                on_heap_ = false;
            }

            void spill()
            {
                heap_.reserve(N * 2);

                for (size_t i = 0; i < size_; ++i)
                {
                    heap_.push_back(std::move(ref(i)));
                    ref(i).~T();
                }

                on_heap_ = true;
            }
        };

    } // namespace detail

    // ── apply_result ─────────────────────────────────────────────────────────────

    struct apply_result
    {
        int actual_send_buf = 0;
        int actual_recv_buf = 0;
    };

    // ── socket_options ───────────────────────────────────────────────────────────

    class socket_options
    {
      public:
        socket_options() noexcept = default;

        // ── Address reuse ────────────────────────────────────────────────────

        socket_options &reuse_addr(bool enable)
        {
            push_int(socket_option_id::reuse_addr, enable ? 1 : 0, true);
            has_reuse_addr_ = true;
            return *this;
        }

        bool has_reuse_addr() const noexcept
        {
            return has_reuse_addr_;
        }

        socket_options &reuse_port(bool enable)
        {
            push_int(socket_option_id::reuse_port, enable ? 1 : 0, has_reuseport);
            return *this;
        }

        socket_options &exclusive_addr(bool enable)
        {
#if defined(SOCKETPP_OS_WINDOWS)
            push_int(socket_option_id::exclusive_addr, enable ? 1 : 0, true);
#else
            (void)enable;
            push_int(socket_option_id::exclusive_addr, 0, false);
#endif
            return *this;
        }

        // ── Buffer sizes ─────────────────────────────────────────────────────

        socket_options &recv_buf(int bytes)
        {
            push_int(socket_option_id::recv_buf, bytes, true);
            has_recv_buf_ = true;
            return *this;
        }

        socket_options &send_buf(int bytes)
        {
            push_int(socket_option_id::send_buf, bytes, true);
            has_send_buf_ = true;
            return *this;
        }

        socket_options &buf_profile(const socketpp::buf_profile &profile)
        {
            send_buf(profile.send_size);
            recv_buf(profile.recv_size);
            return *this;
        }

        // ── TCP tuning ───────────────────────────────────────────────────────

        socket_options &tcp_nodelay(bool enable)
        {
            push_int(socket_option_id::tcp_nodelay, enable ? 1 : 0, true);
            return *this;
        }

        socket_options &tcp_cork(bool enable)
        {
            push_int(socket_option_id::tcp_cork, enable ? 1 : 0, has_tcp_cork);
            return *this;
        }

        socket_options &tcp_notsent_lowat(int bytes)
        {
            push_int(socket_option_id::tcp_notsent_lowat, bytes, has_tcp_notsent_lowat);
            return *this;
        }

        socket_options &tcp_user_timeout(std::chrono::milliseconds timeout)
        {
            push_int(socket_option_id::tcp_user_timeout, static_cast<int>(timeout.count()), has_tcp_user_timeout);
            return *this;
        }

        socket_options &tcp_fastopen(int queue_len)
        {
            push_int(socket_option_id::tcp_fastopen, queue_len, has_tcp_fastopen);
            return *this;
        }

        socket_options &tcp_defer_accept(std::chrono::seconds timeout)
        {
            push_int(socket_option_id::tcp_defer_accept, static_cast<int>(timeout.count()), has_tcp_defer_accept);
            return *this;
        }

        // ── Keep-alive ───────────────────────────────────────────────────────

        socket_options &keep_alive(bool enable)
        {
            push_int(socket_option_id::keep_alive, enable ? 1 : 0, true);
            return *this;
        }

        socket_options &keep_alive_idle(std::chrono::seconds idle)
        {
            push_int(socket_option_id::keep_alive_idle, static_cast<int>(idle.count()), true);
            return *this;
        }

        socket_options &keep_alive_interval(std::chrono::seconds interval)
        {
            push_int(socket_option_id::keep_alive_interval, static_cast<int>(interval.count()), true);
            return *this;
        }

        socket_options &keep_alive_count(int count)
        {
#if defined(SOCKETPP_OS_LINUX) || defined(SOCKETPP_OS_MACOS)
            push_int(socket_option_id::keep_alive_count, count, true);
#else
            (void)count;
            push_int(socket_option_id::keep_alive_count, 0, false);
#endif
            return *this;
        }

        // ── Linger ───────────────────────────────────────────────────────────

        socket_options &linger(bool enable, int seconds)
        {
            option_entry e;
            e.id = socket_option_id::linger_opt;
            e.platform_available = true;
            e.data.resize(sizeof(int) * 2);
            int vals[2] = {enable ? 1 : 0, seconds};
            std::memcpy(e.data.data(), vals, sizeof(vals));
            entries_.push_back(std::move(e));
            return *this;
        }

        // ── IPv6 ─────────────────────────────────────────────────────────────

        socket_options &ipv6_only(bool enable)
        {
            push_int(socket_option_id::ipv6_only, enable ? 1 : 0, true);
            return *this;
        }

        // ── IP ───────────────────────────────────────────────────────────────

        socket_options &ip_tos(int tos)
        {
            push_int(socket_option_id::ip_tos, tos, true);
            return *this;
        }

        socket_options &broadcast(bool enable)
        {
            push_int(socket_option_id::broadcast, enable ? 1 : 0, true);
            return *this;
        }

        // ── Multicast ────────────────────────────────────────────────────────

        socket_options &multicast_join(const sock_address &group, const sock_address &iface)
        {
            push_multicast(socket_option_id::multicast_join, group, iface);
            return *this;
        }

        socket_options &multicast_leave(const sock_address &group, const sock_address &iface)
        {
            push_multicast(socket_option_id::multicast_leave, group, iface);
            return *this;
        }

        socket_options &multicast_ttl(int ttl)
        {
            push_int(socket_option_id::multicast_ttl, ttl, true);
            return *this;
        }

        socket_options &multicast_loop(bool enable)
        {
            push_int(socket_option_id::multicast_loop, enable ? 1 : 0, true);
            return *this;
        }

        socket_options &multicast_ttl_v6(int hops)
        {
            push_int(socket_option_id::multicast_ttl_v6, hops, true);
            return *this;
        }

        socket_options &multicast_loop_v6(bool enable)
        {
            push_int(socket_option_id::multicast_loop_v6, enable ? 1 : 0, true);
            return *this;
        }

        socket_options &multicast_interface(const sock_address &iface)
        {
            option_entry e;
            e.id = socket_option_id::multicast_if;
            e.platform_available = true;
            e.data.resize(128 + sizeof(uint32_t));
            std::memcpy(e.data.data(), iface.data(), iface.size());
            uint32_t len = iface.size();
            std::memcpy(e.data.data() + 128, &len, sizeof(len));
            entries_.push_back(std::move(e));
            return *this;
        }

        socket_options &multicast_interface_v6(unsigned int if_index)
        {
            option_entry e;
            e.id = socket_option_id::multicast_if_v6;
            e.platform_available = true;
            e.data.resize(sizeof(unsigned int));
            std::memcpy(e.data.data(), &if_index, sizeof(if_index));
            entries_.push_back(std::move(e));
            return *this;
        }

        // ── Apply (implemented in options.cpp) ───────────────────────────────

        result<apply_result> apply_to(socket_t handle) const noexcept;

        result<apply_result> apply_pre_bind(socket_t handle) const noexcept;

        result<void> apply_post_bind(socket_t handle) const noexcept;

        result<void> leave_all_multicast(socket_t handle) const noexcept;

        // ── Query ────────────────────────────────────────────────────────────

        bool has_send_buf() const noexcept
        {
            return has_send_buf_;
        }

        bool has_recv_buf() const noexcept
        {
            return has_recv_buf_;
        }

        bool has_post_bind_opts() const noexcept
        {
            return has_multicast_join_;
        }

      private:
        friend result<apply_result> apply_options_impl(socket_t handle, const socket_options &opts) noexcept;

        struct option_entry
        {
            socket_option_id id;
            std::vector<uint8_t> data;
            bool platform_available = true;
        };

        detail::small_vector<option_entry, 16> entries_;

        bool has_send_buf_ = false;
        bool has_recv_buf_ = false;
        bool has_reuse_addr_ = false;
        bool has_multicast_join_ = false;

        void push_int(socket_option_id id, int value, bool available)
        {
            option_entry e;
            e.id = id;
            e.data.resize(sizeof(int));
            std::memcpy(e.data.data(), &value, sizeof(int));
            e.platform_available = available;
            entries_.push_back(std::move(e));
        }

        void push_multicast(socket_option_id id, const sock_address &group, const sock_address &iface)
        {
            if (id == socket_option_id::multicast_join)
                has_multicast_join_ = true;

            option_entry e;
            e.id = id;
            e.platform_available = true;

            constexpr size_t addr_size = 128 + sizeof(uint32_t);

            e.data.resize(addr_size * 2);
            auto *p = e.data.data();

            std::memcpy(p, group.data(), group.size());
            uint32_t glen = group.size();
            std::memcpy(p + 128, &glen, sizeof(glen));

            std::memcpy(p + addr_size, iface.data(), iface.size());
            uint32_t ilen = iface.size();
            std::memcpy(p + addr_size + 128, &ilen, sizeof(ilen));

            entries_.push_back(std::move(e));
        }
    };

} // namespace socketpp

#endif // SOCKETPP_SOCKET_OPTIONS_HPP

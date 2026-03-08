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

/// @file tests.cpp
/// Combined test suite for socketpp.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <socketpp/event/loop.hpp>
#include <socketpp/net/address.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/capabilities.hpp>
#include <socketpp/platform/detect.hpp>
#include <socketpp/platform/types.hpp>
#include <socketpp/socket/buf_profile.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>
#include <socketpp/socket/tcp.hpp>
#include <socketpp/socket/tcp_connector.hpp>
#include <socketpp/socket/tcp_listener.hpp>
#include <socketpp/socket/udp.hpp>
#include <socketpp/socket/udp_peer.hpp>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(SOCKETPP_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace
{

    void platform_sleep_ms(int ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    bool wait_readable(socketpp::socket_t fd, int timeout_ms = 2000)
    {
#if defined(SOCKETPP_OS_WINDOWS)
        WSAPOLLFD pfd = {};
        pfd.fd = static_cast<SOCKET>(fd);
        pfd.events = POLLIN;
        return WSAPoll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
#else
        struct pollfd pfd = {};
        pfd.fd = static_cast<int>(fd);
        pfd.events = POLLIN;
        return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
#endif
    }

    bool wait_writable(socketpp::socket_t fd, int timeout_ms = 2000)
    {
#if defined(SOCKETPP_OS_WINDOWS)
        WSAPOLLFD pfd = {};
        pfd.fd = static_cast<SOCKET>(fd);
        pfd.events = POLLOUT;
        return WSAPoll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
#else
        struct pollfd pfd = {};
        pfd.fd = static_cast<int>(fd);
        pfd.events = POLLOUT;
        return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
#endif
    }

    struct thread_handle
    {
        std::thread t;

        template<typename Fn> static thread_handle create(Fn fn)
        {
            thread_handle th;
            th.t = std::thread(std::move(fn));
            return th;
        }

        void join()
        {
            if (t.joinable())
                t.join();
        }
    };

} // anonymous namespace

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

// Audit fix L7: track per-test fail count for accurate pass/fail
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

// ===========================================================================
// Address tests
// ===========================================================================

void test_inet4_default_construction()
{
    socketpp::inet4_address addr;
    CHECK(addr.port() == 0);
}

void test_inet4_loopback()
{
    auto addr = socketpp::inet4_address::loopback(8080);
    CHECK(addr.port() == 8080);
    auto s = addr.to_string();
    CHECK(s.find("127.0.0.1") != std::string::npos);
    CHECK(s.find("8080") != std::string::npos);
}

void test_inet4_any()
{
    auto addr = socketpp::inet4_address::any(0);
    CHECK(addr.port() == 0);
    auto s = addr.to_string();
    CHECK(s.find("0.0.0.0") != std::string::npos);
}

void test_inet4_parse()
{
    auto r = socketpp::inet4_address::parse("192.168.1.1", 443);
    CHECK(r);
    if (!r)
        return;
    auto addr = r.value();
    CHECK(addr.port() == 443);
    auto s = addr.to_string();
    CHECK(s.find("192.168.1.1") != std::string::npos);
}

void test_inet4_parse_invalid()
{
    auto r = socketpp::inet4_address::parse("not.an.ip.address", 80);
    CHECK(!r);
}

void test_inet4_equality()
{
    auto a = socketpp::inet4_address::loopback(1234);
    auto b = socketpp::inet4_address::loopback(1234);
    auto c = socketpp::inet4_address::loopback(5678);
    CHECK(a == b);
    CHECK(a != c);
}

void test_inet4_hashing()
{
    auto a = socketpp::inet4_address::loopback(1000);
    auto b = socketpp::inet4_address::loopback(1000);
    auto c = socketpp::inet4_address::loopback(2000);

    std::unordered_set<socketpp::inet4_address> set;
    set.insert(a);
    set.insert(b);
    set.insert(c);

    CHECK(set.size() == 2u);
    CHECK(set.count(a));
    CHECK(set.count(c));
}

void test_inet6_loopback()
{
    auto addr = socketpp::inet6_address::loopback(9090);
    CHECK(addr.port() == 9090);
    auto s = addr.to_string();
    CHECK(s.find("::1") != std::string::npos);
}

void test_inet6_any()
{
    auto addr = socketpp::inet6_address::any(0);
    CHECK(addr.port() == 0);
}

void test_inet6_parse()
{
    auto r = socketpp::inet6_address::parse("::1", 8080);
    CHECK(r);
    if (!r)
        return;
    auto addr = r.value();
    CHECK(addr.port() == 8080);
}

void test_inet6_parse_invalid()
{
    auto r = socketpp::inet6_address::parse("not-an-ipv6", 80);
    CHECK(!r);
}

void test_inet6_is_v4_mapped()
{
    auto r = socketpp::inet6_address::parse("::ffff:127.0.0.1", 80);
    if (r)
    {
        CHECK(r.value().is_v4_mapped());
    }
}

void test_inet6_to_v4()
{
    auto r = socketpp::inet6_address::parse("::ffff:192.168.1.1", 3000);
    if (r)
    {
        auto v4_r = r.value().to_v4();
        CHECK(v4_r);
        if (!v4_r)
            return;
        auto s = v4_r.value().to_string();
        CHECK(s.find("192.168.1.1") != std::string::npos);
        CHECK(v4_r.value().port() == 3000);
    }
}

void test_inet6_non_mapped_to_v4_fails()
{
    auto addr = socketpp::inet6_address::loopback(80);
    CHECK(!addr.is_v4_mapped());
}

void test_sock_address_round_trip_v4()
{
    auto orig = socketpp::inet4_address::loopback(7777);
    socketpp::sock_address sa = orig;

    CHECK(sa.is_v4());
    CHECK(sa.size() > 0);

    socketpp::inet4_address reconstructed;
    std::memcpy(&reconstructed, sa.data(), sizeof(reconstructed));
    CHECK(reconstructed == orig);
}

void test_sock_address_round_trip_v6()
{
    auto orig = socketpp::inet6_address::loopback(8888);
    socketpp::sock_address sa = orig;

    CHECK(sa.is_v6());
    CHECK(sa.size() > 0);

    socketpp::inet6_address reconstructed;
    std::memcpy(&reconstructed, sa.data(), sizeof(reconstructed));
    CHECK(reconstructed == orig);
}

void test_sock_address_copy_and_assign()
{
    auto orig = socketpp::inet4_address::loopback(1111);
    socketpp::sock_address a = orig;
    socketpp::sock_address b = a;

    CHECK(a.family() == b.family());
    CHECK(a.size() == b.size());
}

// ===========================================================================
// Socket options tests
// ===========================================================================

void test_buf_profile_localhost()
{
    auto profile = socketpp::buf_profile::localhost();
    (void)profile;
    CHECK(true);
}

void test_buf_profile_wan()
{
    auto profile = socketpp::buf_profile::wan();
    (void)profile;
    CHECK(true);
}

void test_options_builder_chain()
{
    auto opts = socketpp::socket_options {}.reuse_addr(true).tcp_nodelay(true).recv_buf(65536);
    (void)opts;
    CHECK(true);
}

void test_options_default_construction()
{
    socketpp::socket_options opts {};
    CHECK(!opts.has_reuse_addr());
}

void test_options_apply_to_tcp_socket()
{
    auto sock_r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(sock_r, "socket::create failed");
    if (!sock_r)
        return;

    auto sock = std::move(sock_r.value());
    CHECK(sock.is_open());

    auto opts = socketpp::socket_options {}.reuse_addr(true).tcp_nodelay(true);

    socketpp::tcp4_listener listener;
    auto r = listener.open(socketpp::inet4_address::loopback(19900), 1, opts);
    CHECK_MSG(r, "listener.open failed");

    listener.stop();
}

void test_options_reuse_addr_on_udp()
{
    socketpp::udp4_socket sock;
    auto opts = socketpp::socket_options {}.reuse_addr(true);
    auto r = sock.open(socketpp::inet4_address::loopback(19901), opts);
    CHECK_MSG(r, "udp open failed");
    sock.close();
}

void test_options_recv_buf_size()
{
    auto opts = socketpp::socket_options {}.recv_buf(32768);

    socketpp::tcp4_listener listener;
    auto r = listener.open(socketpp::inet4_address::loopback(19902), 1, opts);
    CHECK_MSG(r, "listener.open with recv_buf failed");
    listener.stop();
}

void test_options_reuse_port_availability()
{
    if constexpr (!socketpp::has_reuseport)
    {
        auto opts = socketpp::socket_options {}.reuse_addr(true);
        socketpp::tcp4_listener listener;
        auto r = listener.open(socketpp::inet4_address::loopback(19903), 1, opts);
        CHECK_MSG(r, "listener.open failed (no reuseport)");
        listener.stop();
    }
    else
    {
        auto opts = socketpp::socket_options {}.reuse_addr(true);
        socketpp::tcp4_listener listener;
        auto r = listener.open(socketpp::inet4_address::loopback(19904), 1, opts);
        CHECK_MSG(r, "listener.open failed (reuseport available)");
        listener.stop();
    }
}

// ===========================================================================
// TCP tests
// ===========================================================================

static socketpp::inet4_address open_listener(
    socketpp::tcp4_listener &listener,
    const socketpp::socket_options &opts = socketpp::socket_options {}.reuse_addr(true))
{
    auto r = listener.open(socketpp::inet4_address::loopback(19905), socketpp::default_backlog, opts);
    CHECK_MSG(r, "listener.open failed");

    auto local = listener.handle().local_address();
    CHECK_MSG(local, "local_address failed");

    socketpp::inet4_address addr;
    if (local)
    {
        std::memcpy(&addr, local.value().data(), sizeof(addr));
    }
    return addr;
}

void test_tcp_create_and_close()
{
    auto r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(r, "socket::create failed");
    if (!r)
        return;
    auto sock = std::move(r.value());
    CHECK(sock.is_open());

    sock.close();
    CHECK(!sock.is_open());
}

void test_tcp_double_close()
{
    auto r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(r, "socket::create failed");
    if (!r)
        return;
    auto sock = std::move(r.value());

    sock.close();
    CHECK(!sock.is_open());

    auto r2 = sock.close();
    CHECK(!sock.is_open());
    (void)r2;
}

void test_tcp_listener_open_on_loopback()
{
    socketpp::tcp4_listener listener;
    auto addr = open_listener(listener);
    CHECK(addr.port() != 0);
    listener.stop();
}

void test_tcp_listener_accept_and_connect()
{
    socketpp::tcp4_listener listener;
    auto addr = open_listener(listener);

    auto opts = socketpp::socket_options {}.tcp_nodelay(true);
    auto conn_r = socketpp::tcp4_connector::connect(addr, opts);
    CHECK_MSG(conn_r, "connector failed");
    if (!conn_r)
    {
        listener.stop();
        return;
    }
    auto client = std::move(conn_r.value());

    wait_readable(listener.handle().native_handle());

    socketpp::inet4_address peer;
    auto accept_r = listener.accept(peer);
    CHECK_MSG(accept_r, "accept failed");
    if (!accept_r)
    {
        listener.stop();
        return;
    }
    auto server_side = std::move(accept_r.value());

    CHECK(peer.port() != 0);
    CHECK(client.is_open());
    CHECK(server_side.is_open());

    server_side.close();
    client.close();
    listener.stop();
}

void test_tcp_echo_round_trip()
{
    socketpp::tcp4_listener listener;
    auto addr = open_listener(listener, socketpp::socket_options {}.reuse_addr(true).tcp_nodelay(true));

    auto conn_r = socketpp::tcp4_connector::connect(addr, socketpp::socket_options {}.tcp_nodelay(true));
    CHECK_MSG(conn_r, "connector failed");
    if (!conn_r)
    {
        listener.stop();
        return;
    }
    auto client = std::move(conn_r.value());

    wait_readable(listener.handle().native_handle());

    socketpp::inet4_address peer;
    auto accept_r = listener.accept(peer);
    CHECK_MSG(accept_r, "accept failed");
    if (!accept_r)
    {
        listener.stop();
        return;
    }
    auto server = std::move(accept_r.value());

    wait_writable(client.native_handle());

    const std::string msg = "Hello, socketpp TCP!";
    auto send_r = client.send(msg.data(), msg.size());
    CHECK_MSG(send_r, "client.send failed");
    if (!send_r)
    {
        listener.stop();
        return;
    }
    CHECK(send_r.value() == msg.size());

    wait_readable(server.native_handle());

    std::array<char, 256> buf {};
    auto recv_r = server.recv(buf.data(), buf.size());
    CHECK_MSG(recv_r, "server.recv failed");
    if (!recv_r)
    {
        listener.stop();
        return;
    }
    CHECK(recv_r.value() == msg.size());
    CHECK(std::string(buf.data(), recv_r.value()) == msg);

    auto echo_send = server.send(buf.data(), recv_r.value());
    CHECK_MSG(echo_send, "server echo send failed");

    wait_readable(client.native_handle());

    std::array<char, 256> echo_buf {};
    auto echo_recv = client.recv(echo_buf.data(), echo_buf.size());
    CHECK_MSG(echo_recv, "client echo recv failed");
    if (echo_recv)
    {
        CHECK(std::string(echo_buf.data(), echo_recv.value()) == msg);
    }

    server.close();
    client.close();
    listener.stop();
}

void test_tcp_socket_move_construction()
{
    auto r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(r, "socket::create failed");
    if (!r)
        return;
    auto original = std::move(r.value());
    CHECK(original.is_open());

    auto handle = original.native_handle();

    socketpp::socket moved(std::move(original));
    CHECK(!original.is_open());
    CHECK(moved.is_open());
    CHECK(moved.native_handle() == handle);

    moved.close();
}

void test_tcp_socket_move_assignment()
{
    auto r1 = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    auto r2 = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(r1, "socket::create r1 failed");
    CHECK_MSG(r2, "socket::create r2 failed");
    if (!r1 || !r2)
        return;

    auto sock1 = std::move(r1.value());
    auto sock2 = std::move(r2.value());
    auto handle2 = sock2.native_handle();

    sock1 = std::move(sock2);
    CHECK(sock1.is_open());
    CHECK(!sock2.is_open());
    CHECK(sock1.native_handle() == handle2);

    sock1.close();
}

void test_tcp_listener_move_construction()
{
    socketpp::tcp4_listener listener;
    open_listener(listener);
    CHECK(listener.handle().is_open());

    socketpp::tcp4_listener moved(std::move(listener));
    CHECK(moved.handle().is_open());

    moved.stop();
}

// ===========================================================================
// IPv6 TCP socket lifecycle tests (audit fix L6)
// ===========================================================================

void test_tcp6_create_and_close()
{
    auto r = socketpp::socket::create(socketpp::address_family::ipv6, socketpp::socket_type::stream);
    CHECK_MSG(r, "socket::create ipv6 failed");
    if (!r)
        return;
    auto sock = std::move(r.value());
    CHECK(sock.is_open());

    sock.close();
    CHECK(!sock.is_open());
}

void test_tcp6_listener_accept_and_connect()
{
    socketpp::tcp6_listener listener;
    auto r = listener.open(
        socketpp::inet6_address::loopback(19906),
        socketpp::default_backlog,
        socketpp::socket_options {}.reuse_addr(true).tcp_nodelay(true));
    CHECK_MSG(r, "v6 listener.open failed");
    if (!r)
        return;

    auto local_sa = listener.handle().local_address();
    CHECK_MSG(local_sa, "v6 local_address failed");
    if (!local_sa)
    {
        listener.stop();
        return;
    }

    socketpp::inet6_address local_addr;
    std::memcpy(&local_addr, local_sa.value().data(), sizeof(local_addr));

    auto conn_r = socketpp::tcp6_connector::connect(local_addr, socketpp::socket_options {}.tcp_nodelay(true));
    CHECK_MSG(conn_r, "v6 connector failed");
    if (!conn_r)
    {
        listener.stop();
        return;
    }
    auto client = std::move(conn_r.value());

    wait_readable(listener.handle().native_handle());

    socketpp::inet6_address peer;
    auto accept_r = listener.accept(peer);
    CHECK_MSG(accept_r, "v6 accept failed");
    if (!accept_r)
    {
        listener.stop();
        return;
    }
    auto server = std::move(accept_r.value());

    CHECK(client.is_open());
    CHECK(server.is_open());

    wait_writable(client.native_handle());

    const std::string msg = "IPv6 echo";
    auto send_r = client.send(msg.data(), msg.size());
    CHECK_MSG(send_r, "v6 send failed");

    wait_readable(server.native_handle());

    std::array<char, 128> buf {};
    auto recv_r = server.recv(buf.data(), buf.size());
    CHECK_MSG(recv_r, "v6 recv failed");
    if (recv_r)
    {
        CHECK(std::string(buf.data(), recv_r.value()) == msg);
    }

    server.close();
    client.close();
    listener.stop();
}

void test_udp6_send_recv()
{
    socketpp::udp6_socket sender;
    socketpp::udp6_socket receiver;

    auto r1 = sender.open(socketpp::inet6_address::loopback(19907), socketpp::socket_options {});
    CHECK_MSG(r1, "v6 sender open failed");

    auto r2 = receiver.open(socketpp::inet6_address::loopback(19908), socketpp::socket_options {});
    CHECK_MSG(r2, "v6 receiver open failed");

    auto recv_local = receiver.local_addr();
    CHECK_MSG(recv_local, "v6 receiver local_addr failed");
    if (!recv_local)
    {
        sender.close();
        receiver.close();
        return;
    }

    const std::string msg = "Hello IPv6 UDP!";
    auto send_r = sender.send_to(msg.data(), msg.size(), recv_local.value());
    CHECK_MSG(send_r, "v6 send_to failed");

    wait_readable(receiver.native_handle());

    std::array<char, 1500> buf {};
    socketpp::inet6_address from;
    auto recv_r = receiver.recv_from(buf.data(), buf.size(), from);
    CHECK_MSG(recv_r, "v6 recv_from failed");
    if (recv_r)
    {
        CHECK(recv_r.value() == msg.size());
        CHECK(std::string(buf.data(), recv_r.value()) == msg);
    }

    sender.close();
    receiver.close();
}

// ===========================================================================
// UDP tests
// ===========================================================================

void test_udp_create_and_bind()
{
    socketpp::udp4_socket sock;
    auto r = sock.open(socketpp::inet4_address::loopback(19909), socketpp::socket_options {}.reuse_addr(true));
    CHECK_MSG(r, "udp open failed");
    CHECK(sock.is_open());
    sock.close();
}

void test_udp_bind_any_port()
{
    socketpp::udp4_socket sock;
    auto r = sock.open(socketpp::inet4_address::any(19910), socketpp::socket_options {});
    CHECK_MSG(r, "udp open any failed");
    sock.close();
}

void test_udp_send_to_recv_from()
{
    socketpp::udp4_socket sender;
    socketpp::udp4_socket receiver;

    auto r1 = sender.open(socketpp::inet4_address::loopback(19911), socketpp::socket_options {});
    CHECK_MSG(r1, "sender open failed");

    auto r2 = receiver.open(socketpp::inet4_address::loopback(19912), socketpp::socket_options {});
    CHECK_MSG(r2, "receiver open failed");

    auto recv_local = receiver.local_addr();
    CHECK_MSG(recv_local, "receiver local_addr failed");
    if (!recv_local)
    {
        sender.close();
        receiver.close();
        return;
    }

    const std::string msg = "Hello UDP!";
    auto send_r = sender.send_to(msg.data(), msg.size(), recv_local.value());
    CHECK_MSG(send_r, "send_to failed");
    if (send_r)
    {
        CHECK(send_r.value() == msg.size());
    }

    wait_readable(receiver.native_handle());

    std::array<char, 1500> buf {};
    socketpp::inet4_address from;
    auto recv_r = receiver.recv_from(buf.data(), buf.size(), from);
    CHECK_MSG(recv_r, "recv_from failed");
    if (recv_r)
    {
        CHECK(recv_r.value() == msg.size());
        CHECK(std::string(buf.data(), recv_r.value()) == msg);

        auto from_str = from.to_string();
        CHECK(from_str.find("127.0.0.1") != std::string::npos);
    }

    sender.close();
    receiver.close();
}

void test_udp_zero_length_datagram()
{
    socketpp::udp4_socket sender;
    socketpp::udp4_socket receiver;

    auto r1 = sender.open(socketpp::inet4_address::loopback(19913), socketpp::socket_options {});
    CHECK_MSG(r1, "sender open failed");

    auto r2 = receiver.open(socketpp::inet4_address::loopback(19914), socketpp::socket_options {});
    CHECK_MSG(r2, "receiver open failed");

    auto recv_local = receiver.local_addr();
    CHECK_MSG(recv_local, "receiver local_addr failed");
    if (!recv_local)
    {
        sender.close();
        receiver.close();
        return;
    }

    auto send_r = sender.send_to("", 0, recv_local.value());
    CHECK_MSG(send_r, "send_to zero-length failed");
    if (send_r)
    {
        CHECK(send_r.value() == 0u);
    }

    wait_readable(receiver.native_handle());

    std::array<char, 1500> buf {};
    socketpp::inet4_address from;
    auto recv_r = receiver.recv_from(buf.data(), buf.size(), from);
    CHECK_MSG(recv_r, "recv_from zero-length failed");
    if (recv_r)
    {
        CHECK(recv_r.value() == 0u);
    }

    sender.close();
    receiver.close();
}

void test_udp_batch_recv()
{
    socketpp::udp4_socket sender;
    socketpp::udp4_socket receiver;

    auto r1 = sender.open(socketpp::inet4_address::loopback(19915), socketpp::socket_options {});
    CHECK_MSG(r1, "sender open failed");

    auto r2 = receiver.open(socketpp::inet4_address::loopback(19916), socketpp::socket_options {});
    CHECK_MSG(r2, "receiver open failed");

    auto recv_local = receiver.local_addr();
    CHECK_MSG(recv_local, "receiver local_addr failed");
    if (!recv_local)
    {
        sender.close();
        receiver.close();
        return;
    }

    constexpr int count = 4;
    for (int i = 0; i < count; ++i)
    {
        std::string msg = "batch-" + std::to_string(i);
        auto sr = sender.send_to(msg.data(), msg.size(), recv_local.value());
        CHECK_MSG(sr, "batch send_to failed");
    }

    platform_sleep_ms(10);

    constexpr int max_batch = 8;
    std::array<std::array<char, 256>, max_batch> buffers {};
    std::array<socketpp::msg_batch_entry, max_batch> entries {};
    for (int i = 0; i < max_batch; ++i)
    {
        entries[i].buf = buffers[i].data();
        entries[i].len = 256;
    }

    auto batch_r = receiver.recv_batch(socketpp::span<socketpp::msg_batch_entry>(entries.data(), max_batch));
    CHECK_MSG(batch_r, "recv_batch failed");
    if (batch_r)
    {
        CHECK(batch_r.value() >= 1);
    }

    sender.close();
    receiver.close();
}

void test_udp_peer_socket_create_and_send()
{
    socketpp::udp4_socket remote;
    auto r1 = remote.open(socketpp::inet4_address::loopback(19917), socketpp::socket_options {});
    CHECK_MSG(r1, "remote open failed");

    auto remote_addr = remote.local_addr();
    CHECK_MSG(remote_addr, "remote local_addr failed");
    if (!remote_addr)
    {
        remote.close();
        return;
    }

    auto peer_r = socketpp::udp4_peer_socket::create(
        socketpp::inet4_address::loopback(19918), remote_addr.value(), socketpp::socket_options {});
    CHECK_MSG(peer_r, "peer create failed");
    if (!peer_r)
    {
        remote.close();
        return;
    }
    auto peer = std::move(peer_r.value());

    const std::string msg = "connected-udp";
    auto send_r = peer.send(msg.data(), msg.size());
    CHECK_MSG(send_r, "peer.send failed");
    if (send_r)
    {
        CHECK(send_r.value() == msg.size());
    }

    wait_readable(remote.native_handle());

    std::array<char, 256> buf {};
    socketpp::inet4_address from;
    auto recv_r = remote.recv_from(buf.data(), buf.size(), from);
    CHECK_MSG(recv_r, "remote.recv_from failed");
    if (recv_r)
    {
        CHECK(std::string(buf.data(), recv_r.value()) == msg);
    }

    auto echo_r = remote.send_to(buf.data(), recv_r.value(), from);
    CHECK_MSG(echo_r, "remote echo send_to failed");

#if !defined(SOCKETPP_OS_WINDOWS)
    wait_readable(peer.native_handle());
#endif

    std::array<char, 256> echo_buf {};
    auto echo_recv = peer.recv(echo_buf.data(), echo_buf.size());
    CHECK_MSG(echo_recv, "peer.recv failed");
    if (echo_recv)
    {
        CHECK(std::string(echo_buf.data(), echo_recv.value()) == msg);
    }

    remote.close();
}

// ===========================================================================
// Event loop tests
// ===========================================================================

void test_event_loop_create_and_destroy()
{
    socketpp::event_loop loop;
    CHECK(true);
}

void test_event_loop_post_callback_executes()
{
    socketpp::event_loop loop;
    bool called = false;

    loop.post([&called]() { called = true; });

    loop.post([&loop]() { loop.stop(); });

    loop.run();
    CHECK(called);
}

void test_event_loop_post_multiple()
{
    socketpp::event_loop loop;
    int counter = 0;

    for (int i = 0; i < 5; ++i)
    {
        loop.post([&counter]() { ++counter; });
    }
    loop.post([&loop]() { loop.stop(); });

    loop.run();
    CHECK(counter == 5);
}

void test_event_loop_defer_timer_fires()
{
    socketpp::event_loop loop;
    bool fired = false;

    loop.defer(
        std::chrono::milliseconds(50),
        [&]()
        {
            fired = true;
            loop.stop();
        });

    loop.run();
    CHECK(fired);
}

void test_event_loop_defer_order_by_delay()
{
    socketpp::event_loop loop;
    std::string order;

    loop.defer(
        std::chrono::milliseconds(100),
        [&]()
        {
            order += "B";
            loop.stop();
        });
    loop.defer(std::chrono::milliseconds(30), [&]() { order += "A"; });

    loop.run();
    CHECK(order == "AB");
}

void test_event_loop_repeat_timer_fires_multiple()
{
    socketpp::event_loop loop;
    int count = 0;

    auto handle = loop.repeat(
        std::chrono::milliseconds(20),
        [&]()
        {
            ++count;
            if (count >= 3)
            {
                loop.stop();
            }
        });

    loop.run();
    CHECK(count >= 3);
    (void)handle;
}

void test_event_loop_repeat_timer_cancel()
{
    socketpp::event_loop loop;
    int count = 0;

    auto handle = loop.repeat(std::chrono::milliseconds(20), [&]() { ++count; });

    loop.defer(std::chrono::milliseconds(80), [&]() { handle.cancel(); });
    loop.defer(std::chrono::milliseconds(150), [&]() { loop.stop(); });

    loop.run();
    CHECK(count < 10);
}

void test_event_loop_stop_exits_run()
{
    socketpp::event_loop loop;

    loop.post([&loop]() { loop.stop(); });

    auto start = std::chrono::steady_clock::now();
    loop.run();
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(5));
}

void test_event_loop_run_once()
{
    socketpp::event_loop loop;
    int counter = 0;

    loop.post([&counter]() { ++counter; });

    loop.run_once();
    CHECK(counter >= 1);
}

void test_event_loop_tcp_accept()
{
    socketpp::event_loop loop;
    socketpp::tcp4_listener listener;

    auto opts = socketpp::socket_options {}.reuse_addr(true).tcp_nodelay(true);
    auto r = listener.open(socketpp::inet4_address::loopback(19919), socketpp::default_backlog, opts);
    CHECK_MSG(r, "listener.open failed");
    if (!r)
        return;

    auto local_sa = listener.handle().local_address();
    CHECK_MSG(local_sa, "local_address failed");
    if (!local_sa)
    {
        listener.stop();
        return;
    }

    socketpp::inet4_address local_addr;
    std::memcpy(&local_addr, local_sa.value().data(), sizeof(local_addr));

    bool accepted = false;
    std::string echo_result;
    std::unique_ptr<socketpp::tcp4_socket> server_hold;

    loop.io().add(
        listener.handle().native_handle(),
        socketpp::io_event::readable,
        [&](socketpp::socket_t, socketpp::io_event)
        {
            socketpp::inet4_address peer;
            auto accept_r = listener.accept(peer);
            if (!accept_r)
                return;
            accepted = true;

            server_hold = std::make_unique<socketpp::tcp4_socket>(std::move(accept_r.value()));
            auto *sp = server_hold.get();

            loop.io().add(
                sp->native_handle(),
                socketpp::io_event::readable,
                [sp, &echo_result, &loop](socketpp::socket_t fd, socketpp::io_event)
                {
                    std::array<char, 256> buf {};
                    auto recv_r = sp->recv(buf.data(), buf.size());
                    if (recv_r && recv_r.value() > 0)
                    {
                        echo_result.assign(buf.data(), recv_r.value());
                        sp->send(buf.data(), recv_r.value());
                    }
                    loop.io().remove(fd);
                    sp->close();
                    loop.stop();
                });
        });

    auto client_thread = thread_handle::create(
        [&local_addr]()
        {
            platform_sleep_ms(20);

            auto conn_r = socketpp::tcp4_connector::connect(local_addr, socketpp::socket_options {}.tcp_nodelay(true));
            if (!conn_r)
                return;
            auto client = std::move(conn_r.value());

            if (!wait_writable(client.native_handle()))
                return;

            const std::string msg = "event-loop-test";
            client.send(msg.data(), msg.size());

            if (!wait_readable(client.native_handle()))
            {
                client.close();
                return;
            }

            std::array<char, 256> buf {};
            client.recv(buf.data(), buf.size());
            client.close();
        });

    loop.defer(std::chrono::milliseconds(5000), [&loop]() { loop.stop(); });

    loop.run();
    client_thread.join();

    CHECK(accepted);
    CHECK(echo_result == "event-loop-test");

    listener.stop();
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "socketpp tests\n";

    // Address tests
    std::cerr << "\n--- address ---\n";
    RUN_TEST(test_inet4_default_construction);
    RUN_TEST(test_inet4_loopback);
    RUN_TEST(test_inet4_any);
    RUN_TEST(test_inet4_parse);
    RUN_TEST(test_inet4_parse_invalid);
    RUN_TEST(test_inet4_equality);
    RUN_TEST(test_inet4_hashing);
    RUN_TEST(test_inet6_loopback);
    RUN_TEST(test_inet6_any);
    RUN_TEST(test_inet6_parse);
    RUN_TEST(test_inet6_parse_invalid);
    RUN_TEST(test_inet6_is_v4_mapped);
    RUN_TEST(test_inet6_to_v4);
    RUN_TEST(test_inet6_non_mapped_to_v4_fails);
    RUN_TEST(test_sock_address_round_trip_v4);
    RUN_TEST(test_sock_address_round_trip_v6);
    RUN_TEST(test_sock_address_copy_and_assign);

    // Socket options tests
    std::cerr << "\n--- socket_options ---\n";
    RUN_TEST(test_buf_profile_localhost);
    RUN_TEST(test_buf_profile_wan);
    RUN_TEST(test_options_builder_chain);
    RUN_TEST(test_options_default_construction);
    RUN_TEST(test_options_apply_to_tcp_socket);
    RUN_TEST(test_options_reuse_addr_on_udp);
    RUN_TEST(test_options_recv_buf_size);
    RUN_TEST(test_options_reuse_port_availability);

    // TCP tests
    std::cerr << "\n--- tcp ---\n";
    RUN_TEST(test_tcp_create_and_close);
    RUN_TEST(test_tcp_double_close);
    RUN_TEST(test_tcp_listener_open_on_loopback);
    RUN_TEST(test_tcp_listener_accept_and_connect);
    RUN_TEST(test_tcp_echo_round_trip);
    RUN_TEST(test_tcp_socket_move_construction);
    RUN_TEST(test_tcp_socket_move_assignment);
    RUN_TEST(test_tcp_listener_move_construction);

    // IPv6 TCP/UDP lifecycle tests (audit fix L6)
    std::cerr << "\n--- ipv6 ---\n";
    RUN_TEST(test_tcp6_create_and_close);
    RUN_TEST(test_tcp6_listener_accept_and_connect);
    RUN_TEST(test_udp6_send_recv);

    // UDP tests
    std::cerr << "\n--- udp ---\n";
    RUN_TEST(test_udp_create_and_bind);
    RUN_TEST(test_udp_bind_any_port);
    RUN_TEST(test_udp_send_to_recv_from);
    RUN_TEST(test_udp_zero_length_datagram);
    RUN_TEST(test_udp_batch_recv);
    RUN_TEST(test_udp_peer_socket_create_and_send);

    // Event loop tests
    std::cerr << "\n--- event_loop ---\n";
    RUN_TEST(test_event_loop_create_and_destroy);
    RUN_TEST(test_event_loop_post_callback_executes);
    RUN_TEST(test_event_loop_post_multiple);
    RUN_TEST(test_event_loop_defer_timer_fires);
    RUN_TEST(test_event_loop_defer_order_by_delay);
    RUN_TEST(test_event_loop_repeat_timer_fires_multiple);
    RUN_TEST(test_event_loop_repeat_timer_cancel);
    RUN_TEST(test_event_loop_stop_exits_run);
    RUN_TEST(test_event_loop_run_once);
    RUN_TEST(test_event_loop_tcp_accept);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

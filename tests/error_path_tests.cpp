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

/// @file error_path_tests.cpp
/// Tests for error paths, edge cases, and the result<T> type.

#include "test_harness.hpp"

#include <chrono>
#include <cstring>
#include <socketpp/net/address.hpp>
#include <socketpp/net/inet4.hpp>
#include <socketpp/net/inet6.hpp>
#include <socketpp/platform/error.hpp>
#include <socketpp/socket/buf_profile.hpp>
#include <socketpp/socket/options.hpp>
#include <socketpp/socket/socket.hpp>
#include <socketpp/socket/tcp.hpp>
#include <socketpp/socket/udp.hpp>
#include <string>

// ===========================================================================
// inet4_address parse boundary octets
// ===========================================================================

void test_inet4_parse_boundary_octets()
{
    auto r1 = socketpp::inet4_address::parse("0.0.0.0", 80);
    CHECK_MSG(r1, "0.0.0.0 should parse");

    auto r2 = socketpp::inet4_address::parse("255.255.255.255", 80);
    CHECK_MSG(r2, "255.255.255.255 should parse");

    auto r3 = socketpp::inet4_address::parse("256.0.0.0", 80);
    CHECK_MSG(!r3, "256.0.0.0 should fail");

    auto r4 = socketpp::inet4_address::parse("0.0.0.0.", 80);
    CHECK_MSG(!r4, "trailing dot should fail");

    auto r5 = socketpp::inet4_address::parse("1.2.3", 80);
    CHECK_MSG(!r5, "incomplete address should fail");
}

// ===========================================================================
// inet4_address parse port zero
// ===========================================================================

void test_inet4_parse_port_zero()
{
    auto r = socketpp::inet4_address::parse("127.0.0.1", 0);
    CHECK_MSG(r, "port 0 should parse");

    if (r)
        CHECK(r.value().port() == 0);
}

// ===========================================================================
// inet4_address parse negative-style inputs
// ===========================================================================

void test_inet4_parse_negative_style()
{
    auto r1 = socketpp::inet4_address::parse("-1.0.0.0", 80);
    CHECK_MSG(!r1, "negative octet should fail");

    auto r2 = socketpp::inet4_address::parse("1.2.3.4:80", 80);
    CHECK_MSG(!r2, "embedded colon should fail");

    auto r3 = socketpp::inet4_address::parse("", 80);
    CHECK_MSG(!r3, "empty string should fail");

    auto r4 = socketpp::inet4_address::parse("abc.def.ghi.jkl", 80);
    CHECK_MSG(!r4, "alphabetic octets should fail");
}

// ===========================================================================
// inet6_address parse empty
// ===========================================================================

void test_inet6_parse_empty()
{
    auto r = socketpp::inet6_address::parse("", 80);
    CHECK_MSG(!r, "empty string should fail for IPv6");
}

// ===========================================================================
// inet6_address parse overlong
// ===========================================================================

void test_inet6_parse_overlong()
{
    std::string overlong(300, 'a');
    auto r = socketpp::inet6_address::parse(overlong, 80);
    CHECK_MSG(!r, "overlong string should fail for IPv6");
}

// ===========================================================================
// inet6_address link-local scope_id handling
// ===========================================================================

void test_inet6_link_local_scope_id()
{
    // Link-local without scope_id should fail (or succeed depending on implementation)
    auto r1 = socketpp::inet6_address::parse("fe80::1", 80, 0);

    // Link-local with scope_id should succeed
    auto r2 = socketpp::inet6_address::parse("fe80::1", 80, 1);
    CHECK_MSG(r2, "fe80::1 with scope_id=1 should parse");

    if (r2)
    {
        CHECK(r2.value().is_link_local());
        CHECK(r2.value().scope_id() == 1);
    }
}

// ===========================================================================
// inet6_address v4-mapped roundtrip
// ===========================================================================

void test_inet6_v4_mapped_roundtrip()
{
    auto r = socketpp::inet6_address::parse("::ffff:192.168.1.1", 8080);
    CHECK_MSG(r, "v4-mapped address should parse");

    if (!r)
        return;

    auto &addr = r.value();
    CHECK(addr.is_v4_mapped());

    auto v4_r = addr.to_v4();
    CHECK_MSG(v4_r, "to_v4() should succeed for v4-mapped");

    if (v4_r)
    {
        auto v4_str = v4_r.value().to_string();
        CHECK_MSG(v4_str.find("192.168.1.1") != std::string::npos, "v4 should contain 192.168.1.1");
        CHECK(v4_r.value().port() == 8080);
    }
}

// ===========================================================================
// inet6_address non-mapped to_v4 error
// ===========================================================================

void test_inet6_non_mapped_to_v4_error()
{
    auto r = socketpp::inet6_address::parse("::1", 80);
    CHECK_MSG(r, "::1 should parse");

    if (r)
    {
        CHECK(!r.value().is_v4_mapped());

        auto v4_r = r.value().to_v4();
        CHECK_MSG(!v4_r, "to_v4() on non-mapped should fail");
    }
}

// ===========================================================================
// sock_address copy, assign, equality
// ===========================================================================

void test_sock_address_copy_assign_equality()
{
    auto addr = socketpp::inet4_address::loopback(8080);
    socketpp::sock_address sa = addr;

    // Copy construction
    socketpp::sock_address sa2(sa);
    CHECK(sa == sa2);
    CHECK(!(sa != sa2));

    // Assignment
    socketpp::sock_address sa3;
    sa3 = sa;
    CHECK(sa == sa3);

    // Different address should not be equal
    auto addr2 = socketpp::inet4_address::loopback(9090);
    socketpp::sock_address sa4 = addr2;
    CHECK(sa != sa4);
}

// ===========================================================================
// socket_options empty apply
// ===========================================================================

void test_socket_options_empty_apply()
{
    auto sock_r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(sock_r, "socket create should succeed");

    if (!sock_r)
        return;

    auto &sock = sock_r.value();
    socketpp::socket_options opts;

    auto r = opts.apply_to(sock.native_handle());
    CHECK_MSG(r, "empty options apply should succeed");

    sock.close();
}

// ===========================================================================
// socket_options TCP nodelay + keepalive
// ===========================================================================

void test_socket_options_tcp_nodelay_and_keepalive()
{
    auto sock_r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(sock_r, "socket create should succeed");

    if (!sock_r)
        return;

    auto &sock = sock_r.value();

    socketpp::socket_options opts;
    opts.tcp_nodelay(true)
        .keep_alive(true)
        .keep_alive_idle(std::chrono::seconds(10))
        .keep_alive_interval(std::chrono::seconds(5));

    auto r = opts.apply_to(sock.native_handle());
    CHECK_MSG(r, "nodelay + keepalive apply should succeed");

    sock.close();
}

// ===========================================================================
// socket_options recv/send buffer sizes
// ===========================================================================

void test_socket_options_recv_send_buf()
{
    auto sock_r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(sock_r, "socket create should succeed");

    if (!sock_r)
        return;

    auto &sock = sock_r.value();

    socketpp::socket_options opts;
    opts.recv_buf(65536).send_buf(65536);

    auto r = opts.apply_to(sock.native_handle());
    CHECK_MSG(r, "buffer size apply should succeed");

    if (r)
    {
        CHECK_MSG(r.value().actual_recv_buf > 0, "actual recv buf should be positive");
        CHECK_MSG(r.value().actual_send_buf > 0, "actual send buf should be positive");
    }

    sock.close();
}

// ===========================================================================
// TCP send on closed socket error code
// ===========================================================================

void test_tcp_send_on_closed_error_code()
{
    auto sock_r = socketpp::socket::create(socketpp::address_family::ipv4, socketpp::socket_type::stream);
    CHECK_MSG(sock_r, "socket create should succeed");

    if (!sock_r)
        return;

    auto sock = socketpp::tcp4_socket(std::move(sock_r.value()));
    sock.close();

    auto r = sock.send("hello", 5);
    CHECK_MSG(!r, "send on closed socket should fail");
}

// ===========================================================================
// UDP send on closed socket error code
// ===========================================================================

void test_udp_send_on_closed_error_code()
{
    auto sock_r = socketpp::udp4_socket();

    auto open_r = sock_r.open(socketpp::inet4_address::loopback(0));
    CHECK_MSG(open_r, "udp open should succeed");

    if (!open_r)
        return;

    sock_r.close();

    auto r = sock_r.send_to("hello", 5, socketpp::inet4_address::loopback(12345));
    CHECK_MSG(!r, "send_to on closed socket should fail");
}

// ===========================================================================
// result<void> success
// ===========================================================================

void test_result_void_success()
{
    socketpp::result<void> r;
    CHECK(static_cast<bool>(r));
    CHECK(!r.error());
}

// ===========================================================================
// result<void> error
// ===========================================================================

void test_result_void_error()
{
    socketpp::result<void> r(socketpp::errc::timed_out);
    CHECK(!static_cast<bool>(r));
    CHECK(r.error() == socketpp::make_error_code(socketpp::errc::timed_out));
}

// ===========================================================================
// result<T> move semantics
// ===========================================================================

void test_result_value_move()
{
    auto r1 = socketpp::inet4_address::parse("10.0.0.1", 8080);
    CHECK_MSG(r1, "parse should succeed");

    if (!r1)
        return;

    auto ip_before = r1.value().ip();
    auto port_before = r1.value().port();

    socketpp::result<socketpp::inet4_address> r2(std::move(r1));
    CHECK(static_cast<bool>(r2));
    CHECK(r2.value().ip() == ip_before);
    CHECK(r2.value().port() == port_before);
}

// ===========================================================================
// buf_profile from_bdp clamping
// ===========================================================================

void test_buf_profile_from_bdp()
{
    // Very low BDP -> clamp to 4 KB minimum
    auto low = socketpp::buf_profile::from_bdp(100, 0.001);
    CHECK(low.send_size == 4096);
    CHECK(low.recv_size == 4096);

    // Very high BDP -> clamp to 16 MB maximum
    auto high = socketpp::buf_profile::from_bdp(1e9, 1.0);
    CHECK(high.send_size == 16777216);
    CHECK(high.recv_size == 16777216);

    // Normal BDP (1 Gbps * 50ms RTT = 6.25 MB)
    auto normal = socketpp::buf_profile::from_bdp(125000000, 0.05);
    CHECK(normal.send_size == 6250000);
    CHECK(normal.recv_size == 6250000);
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    std::cerr << "error path tests\n";

    std::cerr << "\n--- inet4 parse boundary octets ---\n";
    RUN_TEST(test_inet4_parse_boundary_octets);

    std::cerr << "\n--- inet4 parse port zero ---\n";
    RUN_TEST(test_inet4_parse_port_zero);

    std::cerr << "\n--- inet4 parse negative style ---\n";
    RUN_TEST(test_inet4_parse_negative_style);

    std::cerr << "\n--- inet6 parse empty ---\n";
    RUN_TEST(test_inet6_parse_empty);

    std::cerr << "\n--- inet6 parse overlong ---\n";
    RUN_TEST(test_inet6_parse_overlong);

    std::cerr << "\n--- inet6 link-local scope_id ---\n";
    RUN_TEST(test_inet6_link_local_scope_id);

    std::cerr << "\n--- inet6 v4-mapped roundtrip ---\n";
    RUN_TEST(test_inet6_v4_mapped_roundtrip);

    std::cerr << "\n--- inet6 non-mapped to_v4 error ---\n";
    RUN_TEST(test_inet6_non_mapped_to_v4_error);

    std::cerr << "\n--- sock_address copy/assign/equality ---\n";
    RUN_TEST(test_sock_address_copy_assign_equality);

    std::cerr << "\n--- socket_options empty apply ---\n";
    RUN_TEST(test_socket_options_empty_apply);

    std::cerr << "\n--- socket_options nodelay + keepalive ---\n";
    RUN_TEST(test_socket_options_tcp_nodelay_and_keepalive);

    std::cerr << "\n--- socket_options recv/send buf ---\n";
    RUN_TEST(test_socket_options_recv_send_buf);

    std::cerr << "\n--- TCP send on closed ---\n";
    RUN_TEST(test_tcp_send_on_closed_error_code);

    std::cerr << "\n--- UDP send on closed ---\n";
    RUN_TEST(test_udp_send_on_closed_error_code);

    std::cerr << "\n--- result<void> success ---\n";
    RUN_TEST(test_result_void_success);

    std::cerr << "\n--- result<void> error ---\n";
    RUN_TEST(test_result_void_error);

    std::cerr << "\n--- result<T> move ---\n";
    RUN_TEST(test_result_value_move);

    std::cerr << "\n--- buf_profile from_bdp ---\n";
    RUN_TEST(test_buf_profile_from_bdp);

    std::cerr << "\n" << g_test_count << " checks, " << g_fail_count << " failures\n";
    return g_fail_count > 0 ? 1 : 0;
}

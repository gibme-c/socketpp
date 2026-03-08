# socketpp

A cross-platform, non-blocking socket library for C++17 with native event loop backends and a callback-driven high-level API.

socketpp gives you TCP servers, TCP clients, and UDP servers that work identically on Linux, macOS, and Windows. Under the hood, each platform uses its native I/O multiplexer -- epoll, kqueue, or IOCP -- but you never touch any of that directly. You register callbacks, call `listen()` or `connect()`, and the library handles the event loop, connection lifecycle, and thread dispatch. All user callbacks run on a configurable thread pool, never on the event loop thread, so your handlers can do real work without blocking I/O.

Both IPv4 and IPv6 are first-class citizens. Every server, client, and connection type has separate IPv4 and IPv6 variants (`tcp4_server`/`tcp6_server`, `tcp4_client`/`tcp6_client`, etc.) with dedicated address types that validate at construction. A lower-level socket API is also available for cases where the high-level wrappers don't fit -- batch UDP receive, custom accept loops, or direct socket option manipulation.

## Features

- **Callback-driven TCP** -- `tcp4_server`, `tcp6_server`, `tcp4_client`, `tcp6_client` with `on_connect`, `on_data`, `on_close`, and `on_error` handlers
- **Callback-driven UDP** -- `udp4_server`, `udp6_server` with `on_message` handler and synchronous `send_to()`
- **Native event backends** -- epoll (Linux), kqueue (macOS), IOCP (Windows); selected at compile time
- **Thread pool dispatch** -- all user callbacks run on a worker pool (defaults to hardware concurrency, minimum 2)
- **Fluent configuration** -- chain options before starting: `server.worker_threads(4).backlog(128).listen(addr)`
- **Portable error handling** -- `result<T>` return type with `socketpp::errc` codes that normalize platform-specific socket errors
- **Socket options builder** -- `socket_options` class covering reuse, keepalive, nodelay, buffer sizes, linger, multicast, and more
- **Batch UDP receive** -- low-level `recv_batch()` for high-throughput datagram processing (`recvmmsg` on Linux, iterative fallback elsewhere)
- **Security hardened** -- stack protectors, control-flow integrity, ASLR/DEP, RELRO, Spectre mitigations across GCC, Clang, and MSVC

### Platform Support

| Platform | Event Backend | Link Dependencies |
|----------|---------------|-------------------|
| Linux | epoll | POSIX sockets (libc) |
| macOS | kqueue | POSIX sockets (libc) |
| Windows | IOCP | `ws2_32`, `mswsock` |

## Getting Started

### Requirements

- CMake 3.10+
- A C++17-capable compiler (GCC, Clang, or MSVC)

### Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `SOCKETPP_BUILD_EXAMPLES` | `OFF` | Build the example programs |
| `SOCKETPP_BUILD_TESTS` | `OFF` | Build the test suite |

### Adding to Your Project

As a subdirectory:

```cmake
add_subdirectory(socketpp)
target_link_libraries(your_target PRIVATE socketpp::socketpp)
```

Or after installing (`cmake --install build --prefix /usr/local`):

```cmake
find_package(socketpp REQUIRED)
target_link_libraries(your_target PRIVATE socketpp::socketpp)
```

## Usage

Include the umbrella header for everything:

```cpp
#include <socketpp.hpp>
```

Or include individual headers:

```cpp
#include <socketpp/tcp_server.hpp>
#include <socketpp/tcp_client.hpp>
#include <socketpp/udp_server.hpp>
```

### TCP Echo Server

```cpp
#include <socketpp.hpp>
#include <iostream>

int main()
{
    using namespace socketpp;

    tcp4_server server;

    server.on_connect([](tcp4_connection &conn) {
        std::cout << "connected: " << conn.peer_addr().to_string() << "\n";

        conn.on_data([&conn](const char *data, size_t len) {
            conn.send(data, len);
        });

        conn.on_close([] {
            std::cout << "disconnected\n";
        });
    });

    auto r = server.listen(inet4_address::loopback(9000));
    if (!r)
    {
        std::cerr << "listen failed: " << r.message() << "\n";
        return 1;
    }

    std::cout << "listening on " << server.listening_address().to_string() << "\n";
    server.run();
    return 0;
}
```

### TCP Client

```cpp
#include <socketpp.hpp>
#include <atomic>
#include <iostream>

int main()
{
    using namespace socketpp;

    std::atomic<bool> done{false};
    tcp4_client client;

    client.on_connect([&done](tcp4_connection &conn) {
        conn.send("hello");

        conn.on_data([&conn, &done](const char *data, size_t len) {
            std::cout << "received: " << std::string(data, len) << "\n";
            conn.close();
            done = true;
        });
    });

    client.on_error([&done](std::error_code ec) {
        std::cerr << "connection error: " << ec.message() << "\n";
        done = true;
    });

    client.connect(inet4_address::loopback(9000));
    client.start();

    while (!done)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    client.stop();
    return 0;
}
```

### UDP Echo Server

```cpp
#include <socketpp.hpp>
#include <iostream>

int main()
{
    using namespace socketpp;

    udp4_server server;

    server.on_message([&server](const char *data, size_t len, const inet4_address &sender) {
        std::cout << "from " << sender.to_string() << ": " << std::string(data, len) << "\n";
        server.send_to(data, len, sender);
    });

    auto r = server.bind(inet4_address::loopback(9001));
    if (!r)
    {
        std::cerr << "bind failed: " << r.message() << "\n";
        return 1;
    }

    server.run();
    return 0;
}
```

## API Reference

### Addresses

`inet4_address` and `inet6_address` are value types representing socket endpoints. Both are hashable, comparable, and printable.

```cpp
// IPv4
auto addr = inet4_address::loopback(8080);         // 127.0.0.1:8080
auto addr = inet4_address::any(8080);              // 0.0.0.0:8080
auto addr = inet4_address::parse("10.0.0.1", 80); // result<inet4_address>

// IPv6
auto addr = inet6_address::loopback(8080);         // [::1]:8080
auto addr = inet6_address::any(8080);              // [::]:8080
auto addr = inet6_address::parse("::1", 8080);    // result<inet6_address>
```

`inet6_address` also provides `is_v4_mapped()`, `to_v4()`, `is_link_local()`, and `scope_id()` for link-local addresses.

### TCP Server

```cpp
tcp4_server server;
server
    .on_connect([](tcp4_connection &conn) { /* new connection */ })
    .on_error([](std::error_code ec) { /* accept error */ })
    .worker_threads(4)          // thread pool size (0 = hardware concurrency)
    .max_write_buffer(8 << 20)  // 8 MB max write queue per connection
    .read_buffer_size(32768)    // 32 KB read buffer per connection
    .backlog(128)               // listen backlog
    .max_connections(10000)     // 0 = unlimited
    .socket_opts(socket_options().reuse_addr(true).tcp_nodelay(true));

auto r = server.listen(inet4_address::any(9000));
server.run();   // block until stop()
// or
server.start(); // background thread
server.stop();  // shut down and join
```

`connection_count()` returns the current number of active connections (thread-safe). `listening_address()` returns the bound address, useful when binding to port 0.

### TCP Client

```cpp
tcp4_client client;
client
    .on_connect([](tcp4_connection &conn) { /* connected */ })
    .on_error([](std::error_code ec) { /* connection failed */ })
    .connect_timeout(std::chrono::milliseconds(5000))
    .socket_opts(socket_options().tcp_nodelay(true));

client.connect(inet4_address::loopback(9000));
client.start();
// ...
client.stop();
```

`connected()` returns whether the client currently has an active connection.

### TCP Connection

Connection objects are passed by reference into `on_connect` callbacks. The server/client keeps the connection alive internally -- safe to capture `&conn` in nested callbacks.

```cpp
conn.on_data([&conn](const char *data, size_t len) {
    conn.send(data, len);       // queue data for writing (thread-safe)
});

conn.on_close([] {
    // connection closed (by peer or by you)
});

conn.on_error([](std::error_code ec) {
    // I/O error on this connection
});

conn.close();                   // initiate graceful close
conn.is_open();                 // check connection state
conn.peer_addr();               // remote address
conn.local_addr();              // local address
conn.write_queue_bytes();       // pending outbound bytes (thread-safe)
```

`send()` returns `false` if the write queue exceeds `max_write_buffer`.

### UDP Server

```cpp
udp4_server server;
server
    .on_message([&server](const char *data, size_t len, const inet4_address &sender) {
        server.send_to(data, len, sender); // synchronous send
    })
    .on_error([](std::error_code ec) { /* recv error */ })
    .worker_threads(4)
    .read_buffer_size(65536)
    .socket_opts(socket_options().reuse_addr(true));

auto r = server.bind(inet4_address::any(9001));
server.run();
```

`send_to()` is synchronous (not queued) and returns `true` on success.

### Socket Options

`socket_options` is a builder-style class applied via `.socket_opts()` on any server or client:

```cpp
socket_options opts;
opts.reuse_addr(true)
    .tcp_nodelay(true)
    .keep_alive(true)
    .keep_alive_idle(60)
    .keep_alive_interval(10)
    .keep_alive_count(3)
    .recv_buf(262144)
    .send_buf(262144)
    .linger_opt(true, 5);
```

Options include `reuse_addr`, `reuse_port`, `exclusive_addr`, `tcp_nodelay`, `tcp_cork`, `tcp_fastopen`, `tcp_defer_accept`, `tcp_user_timeout`, `tcp_notsent_lowat`, `keep_alive` (with idle/interval/count), `recv_buf`, `send_buf`, `linger_opt`, `ipv6_only`, `ip_tos`, `broadcast`, and multicast options. Not all options are supported on all platforms -- unsupported options return `errc::option_not_supported`.

### Error Handling

Fallible operations return `result<T>`. Test with `operator bool`, extract the value with `.value()`, or get the error with `.error()` / `.message()`:

```cpp
auto r = server.listen(addr);
if (!r)
{
    std::cerr << r.message() << "\n";   // human-readable string
    std::error_code ec = r.error();     // std::error_code
}
```

`socketpp::errc` provides portable error codes (`would_block`, `connection_reset`, `connection_refused`, `address_in_use`, `timed_out`, `fd_limit_reached`, etc.) that normalize platform differences between POSIX `errno` and Windows `WSAGetLastError()`.

## Architecture

### Event Loop

Each server or client owns a single event loop backed by the platform's native I/O multiplexer. The event loop runs on either the calling thread (`run()`) or a dedicated background thread (`start()`). It monitors sockets for readability and writability, dispatching completions to the thread pool.

The event loop never executes user callbacks directly. All `on_connect`, `on_data`, `on_close`, `on_error`, and `on_message` handlers are posted to the thread pool, keeping the I/O path free of application latency.

### Connection Lifetime

TCP connections are reference-counted internally (`shared_ptr` + `enable_shared_from_this`). The server's connection map holds a `shared_ptr<Connection>` that keeps the object alive for the duration of any in-flight callbacks. When a connection closes, it is removed from the map after all pending callbacks complete. This means `&conn` references captured in callbacks remain valid for the lifetime of those callbacks.

### Threading Model

- **Event loop thread** -- one per server/client, handles I/O multiplexing only
- **Thread pool** -- configurable via `worker_threads()`, defaults to `std::thread::hardware_concurrency()` (minimum 2); all user callbacks execute here
- **`send()` thread safety** -- write data can be queued from any thread; the event loop picks it up on the next writable notification

### Low-Level API

The high-level servers and clients are built on a lower-level socket API that is also public:

- `tcp4_socket` / `tcp6_socket` -- non-blocking TCP sockets with `connect_async()`, `accept()`, `send()`, `recv()`
- `udp4_socket` / `udp6_socket` -- non-blocking UDP sockets with `send_to()`, `recv_from()`, `recv_batch()`
- `tcp4_listener` / `tcp6_listener` -- listening sockets with bind, listen, and accept loop
- `socket_options` -- applied to any socket via the options API
- `event_loop` -- platform-abstracted I/O event loop with `add()`, `modify()`, `remove()`, and `run()`

## Testing

Build with `-DSOCKETPP_BUILD_TESTS=ON` (on by default) to get the test executables:

```bash
./build/tests/tests                # Linux / macOS / MinGW
./build/tests/Release/tests        # Windows (MSVC)
```

The test suite includes low-level socket tests, event loop integration tests, and high-level API tests covering TCP server/client and UDP server round-trips.

## License

BSD-3-Clause. See [LICENSE](LICENSE) for the full text.

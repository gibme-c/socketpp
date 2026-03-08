# Socket++

A cross-platform, non-blocking socket library for C++17 with native event loop backends and a callback-driven high-level API.

Socket++ gives you TCP streams and UDP datagrams that work identically on Linux, macOS, and Windows. Under the hood, each platform uses its native I/O multiplexer -- epoll, kqueue, or IOCP -- but you never touch any of that directly. You register callbacks, call a factory method, and the library handles the event loop, connection lifecycle, and thread dispatch. All user callbacks run on a configurable thread pool, never on the event loop thread, so your handlers can do real work without blocking I/O.

Both IPv4 and IPv6 are first-class citizens. Every type has separate IPv4 and IPv6 variants (`stream4`/`stream6`, `dgram4`/`dgram6`) with dedicated address types that validate at construction.

## Features

- **TCP streams** -- `stream4`, `stream6` with `on_connect`, `on_data`, `on_close`, and `on_error` handlers; unified type for both server (`listen`) and client (`connect`) roles
- **UDP datagrams** -- `dgram4`, `dgram6` with `on_data` handler and synchronous `send_to()`; no client/server distinction
- **Native event backends** -- epoll (Linux), kqueue (macOS), IOCP (Windows); selected at compile time
- **RAII lifecycle** -- factory methods open sockets and start the event loop; destructors clean everything up; no `start()`/`stop()`/`run()` needed
- **Flow control** -- `pause()`/`resume()` on streams, connections, and datagrams backed by kernel buffers
- **Thread pool dispatch** -- all user callbacks run on a worker pool (defaults to hardware concurrency, minimum 2)
- **Portable error handling** -- `result<T>` return type with `socketpp::errc` codes that normalize platform-specific socket errors
- **Socket options builder** -- `socket_options` class covering reuse, keepalive, nodelay, buffer sizes, linger, multicast, and more
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

```cmake
add_subdirectory(socketpp)
target_link_libraries(your_target PRIVATE socketpp)
```

## Usage

Include the umbrella header:

```cpp
#include <socketpp.hpp>
```

This provides the full high-level API: `stream4`, `stream6`, `dgram4`, `dgram6`, address types (`inet4_address`, `inet6_address`), `socket_options`, `result<T>`, and error codes.

### TCP Echo Server

```cpp
#include <socketpp.hpp>
#include <iostream>
#include <thread>

int main()
{
    auto r = socketpp::stream4::listen(socketpp::inet4_address::loopback(9000));
    if (!r)
    {
        std::cerr << "listen failed: " << r.message() << "\n";
        return 1;
    }

    auto server = std::move(r.value());

    server.on_connect([](socketpp::stream4::connection &conn) {
        std::cout << "connected: " << conn.peer_addr().to_string() << "\n";

        conn.on_data([&conn](const char *data, size_t len) {
            conn.send(data, len);
        });

        conn.on_close([] {
            std::cout << "disconnected\n";
        });
    });

    std::cout << "listening on " << server.local_addr().to_string() << "\n";

    // Server runs until destroyed. Sleep to keep main alive.
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
```

### TCP Client

```cpp
#include <socketpp.hpp>
#include <atomic>
#include <iostream>
#include <thread>

int main()
{
    std::atomic<bool> done{false};

    auto r = socketpp::stream4::connect(socketpp::inet4_address::loopback(9000));
    if (!r)
    {
        std::cerr << "connect setup failed: " << r.message() << "\n";
        return 1;
    }

    auto client = std::move(r.value());

    client
        .on_error([&done](std::error_code ec) {
            std::cerr << "connect failed: " << ec.message() << "\n";
            done = true;
        })
        .on_connect([&done](socketpp::stream4::connection &conn) {
            conn.send("hello");

            conn.on_data([&done](const char *data, size_t len) {
                std::cout << "received: " << std::string(data, len) << "\n";
                done = true;
            });
        });

    while (!done)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return 0;
}
```

### UDP Echo Server

```cpp
#include <socketpp.hpp>
#include <iostream>
#include <thread>

int main()
{
    auto r = socketpp::dgram4::create(socketpp::inet4_address::loopback(9001));
    if (!r)
    {
        std::cerr << "create failed: " << r.message() << "\n";
        return 1;
    }

    auto server = std::move(r.value());

    server.on_data([&server](const char *data, size_t len, const socketpp::inet4_address &from) {
        std::cout << "from " << from.to_string() << ": " << std::string(data, len) << "\n";
        server.send_to(data, len, from);
    });

    // Server runs until destroyed.
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
```

## API Reference

### Factory Methods

All high-level types are created via static factory methods that return `result<T>`. Constructors are private. The event loop starts immediately on a background thread, but operations are not armed until callbacks are registered via `on_connect()` or `on_data()`, eliminating races between factory return and callback setup.

```cpp
auto r = socketpp::stream4::listen(addr, config);  // result<stream4>
if (!r)
{
    std::cerr << r.message() << "\n";
    return 1;
}

auto server = std::move(r.value());
```

The same pattern applies to all factory methods:

| Factory | Returns |
|---------|---------|
| `stream4::listen(addr, config)` | `result<stream4>` |
| `stream4::connect(addr, config)` | `result<stream4>` |
| `dgram4::create(addr, config)` | `result<dgram4>` |

IPv6 variants (`stream6`, `dgram6`) follow the same signatures with `inet6_address`.

### Lifecycle

Objects are RAII-managed. The factory opens the socket and starts the event loop; the destructor stops the loop, joins the background thread, and closes the socket. There is no `start()`, `stop()`, or `run()`.

### stream4 / stream6 (TCP)

A unified type for both server and client TCP connections. The role is determined by which factory method you use.

**Listen mode (server):**

```cpp
auto r = socketpp::stream4::listen(
    socketpp::inet4_address::any(9000),
    socketpp::stream_listen_config{
        .worker_threads = 4,          // thread pool size (0 = auto)
        .max_write_buffer = 8 << 20,  // 8 MB max write queue per connection
        .read_buffer_size = 32768,    // 32 KB read buffer per connection
        .sock_opts = socketpp::socket_options{}.reuse_addr(true).tcp_nodelay(true),
        .backlog = 128,               // listen backlog
        .max_connections = 10000      // 0 = unlimited
    });
if (!r) { /* handle error */ return 1; }

auto server = std::move(r.value());

server.on_connect([](socketpp::stream4::connection &conn) {
    // new connection established
});

server.on_error([](std::error_code ec) {
    // accept error
});

server.connection_count(); // current active connections
server.local_addr();       // bound address (useful with ephemeral port)
server.pause();            // stop accepting (kernel backlog buffers pending)
server.resume();           // resume accepting
```

**Connect mode (client):**

```cpp
auto r = socketpp::stream4::connect(
    socketpp::inet4_address::loopback(9000),
    socketpp::stream_connect_config{
        .worker_threads = 0,
        .sock_opts = socketpp::socket_options{}.tcp_nodelay(true),
        .connect_timeout = std::chrono::milliseconds(5000)
    });
if (!r) { /* handle error */ return 1; }

auto client = std::move(r.value());

client
    .on_error([](std::error_code ec) { /* connect failed */ })
    .on_connect([](socketpp::stream4::connection &conn) {
        // connected -- set up handlers and start communicating
    });
```

### stream4::connection / stream6::connection

Connection objects are passed by reference into `on_connect` callbacks. The stream keeps the connection alive internally -- safe to capture `&conn` in nested callbacks.

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
conn.pause();                   // stop reading (TCP flow control throttles sender)
conn.resume();                  // resume reading
conn.paused();                  // check pause state
```

`send()` returns `false` if the write queue exceeds `max_write_buffer`.

### dgram4 / dgram6 (UDP)

A unified datagram type with no client/server role distinction. Role is a usage pattern: bind to a known port and receive (server), or bind to an ephemeral port and send (client) -- same socket, same type.

```cpp
auto r = socketpp::dgram4::create(
    socketpp::inet4_address::any(9001),
    socketpp::dgram_config{
        .worker_threads = 4,         // thread pool size (0 = auto)
        .sock_opts = socketpp::socket_options{}.reuse_addr(true),
        .read_buffer_size = 65536    // >= largest expected datagram
    });
if (!r) { /* handle error */ return 1; }

auto sock = std::move(r.value());

sock.on_data([&sock](const char *data, size_t len, const socketpp::inet4_address &from) {
    sock.send_to(data, len, from); // synchronous send (not queued)
});

sock.on_error([](std::error_code ec) {
    // recv error
});

sock.local_addr();  // bound address (useful with ephemeral port)
sock.pause();       // stop reading (kernel buffer absorbs; drops when full)
sock.resume();      // resume reading
sock.paused();      // check pause state
```

`send_to()` is synchronous and returns `true` on success. Port 0 is supported for ephemeral port binding.

### Addresses

`inet4_address` and `inet6_address` are value types representing socket endpoints. Both are hashable, comparable, and printable.

```cpp
// IPv4
auto addr = socketpp::inet4_address::loopback(8080);         // 127.0.0.1:8080
auto addr = socketpp::inet4_address::any(8080);              // 0.0.0.0:8080
auto addr = socketpp::inet4_address::parse("10.0.0.1", 80); // result<inet4_address>

// IPv6
auto addr = socketpp::inet6_address::loopback(8080);         // [::1]:8080
auto addr = socketpp::inet6_address::any(8080);              // [::]:8080
auto addr = socketpp::inet6_address::parse("::1", 8080);    // result<inet6_address>
```

`inet6_address` also provides `is_v4_mapped()`, `to_v4()`, `is_link_local()`, and `scope_id()` for link-local addresses.

### Socket Options

`socket_options` is a builder-style class applied via config structs:

```cpp
socketpp::socket_options opts;
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
auto r = socketpp::stream4::listen(addr);
if (!r)
{
    std::cerr << r.message() << "\n";   // human-readable string
    std::error_code ec = r.error();     // std::error_code
}
```

`socketpp::errc` provides portable error codes (`would_block`, `connection_reset`, `connection_refused`, `address_in_use`, `timed_out`, `fd_limit_reached`, etc.) that normalize platform differences between POSIX `errno` and Windows `WSAGetLastError()`.

## Architecture

### Event Loop

Each high-level object owns a single event loop backed by the platform's native I/O multiplexer. The event loop runs on a dedicated background thread started by the factory method. It monitors sockets for readability and writability, dispatching completions to the thread pool.

The event loop never executes user callbacks directly. All `on_connect`, `on_data`, `on_close`, `on_error` handlers are posted to the thread pool, keeping the I/O path free of application latency.

### Connection Lifetime

TCP connections are reference-counted internally (`shared_ptr` + `enable_shared_from_this`). The stream's connection map holds a `shared_ptr` that keeps the object alive for the duration of any in-flight callbacks. When a connection closes, it is removed from the map after all pending callbacks complete. This means `&conn` references captured in callbacks remain valid for the lifetime of those callbacks.

### Threading Model

- **Event loop thread** -- one per high-level object, handles I/O multiplexing only
- **Thread pool** -- configurable via `worker_threads` in config structs, defaults to `std::thread::hardware_concurrency()` (minimum 2); all user callbacks execute here
- **`send()` thread safety** -- write data can be queued from any thread; the event loop picks it up on the next writable notification

### Callback Arming

Factory methods start the event loop but do not register sockets for I/O. The `on_connect()` call (for streams) or `on_data()` call (for datagrams) posts the registration to the event loop thread. This eliminates the race between factory return and callback setup -- callbacks are always set before data delivery begins.

## Testing

Build with `-DSOCKETPP_BUILD_TESTS=ON` to get the test executables:

```bash
./build/tests           # all platforms and compilers
```

The test suite includes low-level socket tests, event loop integration tests, and high-level API tests covering TCP streams and UDP datagrams with round-trip verification, pause/resume flow control, and concurrent access patterns.

## License

BSD-3-Clause. See [LICENSE](LICENSE) for the full text.

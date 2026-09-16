// Thin POSIX socket helpers: UDP (unicast and multicast) and TCP, IPv4 only.
//
// Portable between macOS and Linux on purpose. This is the baseline receive
// path, recvfrom on a plain socket, and it is also what runs the whole pipeline
// over loopback on the Mac. The Linux-only paths (recvmmsg, epoll, io_uring,
// AF_XDP) are built on top of the same sockets later.
//
// Errors that mean the setup is wrong throw std::system_error. Errors on the
// data path are returned, because a receiver that throws on one bad datagram
// is a receiver that stops.
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "itch/core/types.hpp"
#include "ttt/net/endpoint.hpp"

namespace ttt::net {

using itch::u8;

// Owns a file descriptor.
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd();
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int  get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void               reset() noexcept;

private:
    int fd_ = -1;
};

using Ipv4 = std::array<u8, 4>;
inline constexpr Ipv4 kLoopback{127, 0, 0, 1};
inline constexpr Ipv4 kAny{0, 0, 0, 0};

struct UdpOptions {
    Endpoint bind{};             // local address and port; port 0 picks one
    Ipv4     iface = kLoopback;  // interface for multicast joins and sends
    bool     join = false;       // join bind.ip as a multicast group
    int      rcvbuf = 0;         // receive buffer if non-zero; see effective_rcvbuf()
    int      sndbuf = 0;
    u8       ttl = 1;
    bool     loop = true;  // deliver our own multicast sends locally
};

// A non-blocking UDP socket, bound, joined to its group if asked.
[[nodiscard]] Fd udp_socket(const UdpOptions& opt);

// The receive buffer the kernel actually granted. Linux silently caps SO_RCVBUF
// at net.core.rmem_max, so asking is not the same as getting; udp_socket tries
// SO_RCVBUFFORCE first, which ignores the cap when the process may.
[[nodiscard]] int effective_rcvbuf(int fd);

// The address a socket ended up bound to.
[[nodiscard]] Endpoint local_endpoint(int fd);

enum class Io : u8 { Ok, WouldBlock, Error };

[[nodiscard]] Io send_to(int fd, const Endpoint& to, std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Io recv_from(int fd, std::span<std::byte> buf, std::size_t& got,
                           Endpoint* from = nullptr) noexcept;

// TCP. The listener and accepted sockets are non-blocking.
[[nodiscard]] Fd tcp_listen(const Endpoint& at, int backlog = 16);
[[nodiscard]] Fd tcp_accept(int listen_fd);  // invalid Fd if none pending

// Starts a non-blocking connect. The socket becomes writable when it finishes;
// connect_result() then says whether it worked.
[[nodiscard]] Fd  tcp_connect_start(const Endpoint& to);
[[nodiscard]] int connect_result(int fd) noexcept;  // 0 or an errno

// Writes everything, waiting as needed. Returns false on error.
[[nodiscard]] bool write_all(int fd, std::span<const std::byte> bytes) noexcept;

// One non-blocking read. got is 0 at end of stream.
[[nodiscard]] Io read_some(int fd, std::span<std::byte> buf, std::size_t& got) noexcept;

void set_nonblocking(int fd, bool on);

}  // namespace ttt::net

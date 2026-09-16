#include "ttt/net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

namespace ttt::net {

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in to_sockaddr(const Endpoint& e) noexcept {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(e.port);
    std::memcpy(&sa.sin_addr, e.ip.data(), 4);
    return sa;
}

Endpoint from_sockaddr(const sockaddr_in& sa) noexcept {
    Endpoint e;
    std::memcpy(e.ip.data(), &sa.sin_addr, 4);
    e.port = ntohs(sa.sin_port);
    return e;
}

in_addr to_in_addr(const Ipv4& ip) noexcept {
    in_addr a{};
    std::memcpy(&a, ip.data(), 4);
    return a;
}

void set_int(int fd, int level, int name, int value, const char* what) {
    if (::setsockopt(fd, level, name, &value, sizeof(value)) != 0) {
        fail(what);
    }
}

#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    set_int(fd, SOL_SOCKET, SO_NOSIGPIPE, 1, "setsockopt(SO_NOSIGPIPE)");
#else
    (void)fd;
#endif
}

}  // namespace

Fd::~Fd() { reset(); }

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        reset();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void set_nonblocking(int fd, bool on) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) != 0) {
        fail("fcntl(O_NONBLOCK)");
    }
}

Fd udp_socket(const UdpOptions& opt) {
    Fd s(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!s.valid()) {
        fail("socket(UDP)");
    }
    // Several receivers may share a multicast port, one per group.
    set_int(s.get(), SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt(SO_REUSEADDR)");
#ifdef SO_REUSEPORT
    set_int(s.get(), SOL_SOCKET, SO_REUSEPORT, 1, "setsockopt(SO_REUSEPORT)");
#endif
    if (opt.rcvbuf != 0) {
        set_int(s.get(), SOL_SOCKET, SO_RCVBUF, opt.rcvbuf, "setsockopt(SO_RCVBUF)");
    }
    if (opt.sndbuf != 0) {
        set_int(s.get(), SOL_SOCKET, SO_SNDBUF, opt.sndbuf, "setsockopt(SO_SNDBUF)");
    }

    // Multicast sends leave through the chosen interface.
    const in_addr iface = to_in_addr(opt.iface);
    if (::setsockopt(s.get(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) != 0) {
        fail("setsockopt(IP_MULTICAST_IF)");
    }
    const unsigned char ttl = opt.ttl;
    const unsigned char loop = opt.loop ? 1 : 0;
    if (::setsockopt(s.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0 ||
        ::setsockopt(s.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0) {
        fail("setsockopt(IP_MULTICAST_TTL/LOOP)");
    }

    // Binding a receiver to the group address, not INADDR_ANY, means a socket
    // for group A never sees group B's traffic on the same port.
    const sockaddr_in sa = to_sockaddr(opt.bind);
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        fail("bind(UDP)");
    }
    if (opt.join) {
        ip_mreq m{};
        m.imr_multiaddr = to_in_addr(opt.bind.ip);
        m.imr_interface = iface;
        if (::setsockopt(s.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0) {
            fail("setsockopt(IP_ADD_MEMBERSHIP)");
        }
    }
    set_nonblocking(s.get(), true);
    return s;
}

Endpoint local_endpoint(int fd) {
    sockaddr_in sa{};
    socklen_t   len = sizeof(sa);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
        fail("getsockname");
    }
    return from_sockaddr(sa);
}

Io send_to(int fd, const Endpoint& to, std::span<const std::byte> bytes) noexcept {
    const sockaddr_in sa = to_sockaddr(to);
    for (;;) {
        const ssize_t n = ::sendto(fd, bytes.data(), bytes.size(), kNoSignal,
                                   reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (n >= 0) {
            return Io::Ok;
        }
        if (errno == EINTR) {
            continue;
        }
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) ? Io::WouldBlock
                                                                             : Io::Error;
    }
}

Io recv_from(int fd, std::span<std::byte> buf, std::size_t& got, Endpoint* from) noexcept {
    sockaddr_in sa{};
    socklen_t   len = sizeof(sa);
    for (;;) {
        const ssize_t n =
            ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&sa), &len);
        if (n >= 0) {
            got = static_cast<std::size_t>(n);
            if (from != nullptr) {
                *from = from_sockaddr(sa);
            }
            return Io::Ok;
        }
        if (errno == EINTR) {
            continue;
        }
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? Io::WouldBlock : Io::Error;
    }
}

Fd tcp_listen(const Endpoint& at, int backlog) {
    Fd s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        fail("socket(TCP)");
    }
    set_int(s.get(), SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt(SO_REUSEADDR)");
    const sockaddr_in sa = to_sockaddr(at);
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        fail("bind(TCP)");
    }
    if (::listen(s.get(), backlog) != 0) {
        fail("listen");
    }
    set_nonblocking(s.get(), true);
    return s;
}

Fd tcp_accept(int listen_fd) {
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            Fd c(fd);
            no_sigpipe(fd);
            set_nonblocking(fd, true);
            return c;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
            return Fd();
        }
        fail("accept");
    }
}

Fd tcp_connect_start(const Endpoint& to) {
    Fd s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        fail("socket(TCP)");
    }
    no_sigpipe(s.get());
    set_nonblocking(s.get(), true);
    const sockaddr_in sa = to_sockaddr(to);
    if (::connect(s.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0 &&
        errno != EINPROGRESS) {
        // Refused right away, e.g. nothing listening. The caller sees the
        // socket become writable and connect_result() report the error.
        if (errno != ECONNREFUSED) {
            fail("connect");
        }
    }
    return s;
}

int connect_result(int fd) noexcept {
    int       err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return errno;
    }
    return err;
}

bool write_all(int fd, std::span<const std::byte> bytes) noexcept {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + done, bytes.size() - done, kNoSignal);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, 1000) <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

Io read_some(int fd, std::span<std::byte> buf, std::size_t& got) noexcept {
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n >= 0) {
            got = static_cast<std::size_t>(n);
            return Io::Ok;
        }
        if (errno == EINTR) {
            continue;
        }
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? Io::WouldBlock : Io::Error;
    }
}

}  // namespace ttt::net

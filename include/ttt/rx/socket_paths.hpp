// Linux receive paths on a plain UDP socket: recvfrom, recvmmsg, and epoll in
// level-triggered and edge-triggered mode.
//
// Every path has the same shape, so one measured receiver loop drives all of
// them and the path is the only variable:
//
//   std::size_t receive(F&& on_datagram)
//
// It waits at most the path's timeout, calls on_datagram(bytes, t_recv) for
// each datagram it got, and returns how many. t_recv is read once, right after
// the call that returned data, and shared by every datagram in that batch:
// that is the moment user space has them, which is what is being measured.
//
// Each path also counts the system calls it makes itself. Those counts are the
// path's own bookkeeping, meant to explain the latency differences; the
// authoritative counts on the measurement box come from perf.
#pragma once

#ifndef __linux__
#error "Linux receive paths"
#endif

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <system_error>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/net/socket.hpp"

namespace ttt::rx {

using itch::u64;

inline constexpr std::size_t kDatagramCapacity = 2048;  // a standard MTU plus margin

struct PathStats {
    u64 syscalls = 0;  // receive-side calls this path made
    u64 wakeups = 0;   // calls that returned data
    u64 empty = 0;     // calls that timed out or found nothing
    u64 datagrams = 0;
    u64 truncated = 0;  // datagrams larger than the buffer
    u64 errors = 0;
};

// A blocking socket with a receive timeout, for the paths that block in the
// receive call itself.
inline void set_blocking_with_timeout(int fd, u64 timeout_ns) {
    net::set_nonblocking(fd, false);
    timeval tv{static_cast<time_t>(timeout_ns / 1'000'000'000u),
               static_cast<suseconds_t>((timeout_ns % 1'000'000'000u) / 1000u)};
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 1;  // zero would mean "block forever"
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt(SO_RCVTIMEO)");
    }
}

// One blocking recvfrom per datagram: one system call, one copy, per packet.
class RecvfromPath {
public:
    RecvfromPath(int fd, u64 timeout_ns) : fd_(fd), buf_(kDatagramCapacity) {
        set_blocking_with_timeout(fd, timeout_ns);
    }

    template <class F>
    std::size_t receive(F&& on_datagram) {
        ++stats_.syscalls;
        const ssize_t n = ::recv(fd_, buf_.data(), buf_.size(), MSG_TRUNC);
        if (n < 0) {
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? ++stats_.empty
                                                                        : ++stats_.errors;
            return 0;
        }
        const u64 t = now_ns();
        ++stats_.wakeups;
        return deliver(static_cast<std::size_t>(n), t, on_datagram);
    }

    [[nodiscard]] const PathStats& stats() const noexcept { return stats_; }

private:
    template <class F>
    std::size_t deliver(std::size_t n, u64 t, F& on_datagram) {
        ++stats_.datagrams;
        if (n > buf_.size()) {
            ++stats_.truncated;
            return 1;
        }
        on_datagram(std::span<const std::byte>(buf_.data(), n), t);
        return 1;
    }

    int                    fd_;
    std::vector<std::byte> buf_;
    PathStats              stats_{};
};

// recvmmsg with MSG_WAITFORONE: blocks for the first datagram, then takes
// whatever else is already queued, up to `batch`, in the same call. Same copy
// per packet as recvfrom, but the system call is shared across the batch.
class RecvmmsgPath {
public:
    RecvmmsgPath(int fd, u64 timeout_ns, unsigned batch)
        : fd_(fd), batch_(batch), bufs_(batch * kDatagramCapacity), iov_(batch), msgs_(batch) {
        set_blocking_with_timeout(fd, timeout_ns);
        for (unsigned i = 0; i < batch; ++i) {
            iov_[i].iov_base = bufs_.data() + i * kDatagramCapacity;
            iov_[i].iov_len = kDatagramCapacity;
            msgs_[i].msg_hdr = msghdr{};
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
        }
    }

    template <class F>
    std::size_t receive(F&& on_datagram) {
        ++stats_.syscalls;
        // The timeout argument of recvmmsg is only checked between datagrams,
        // so the socket's SO_RCVTIMEO is what bounds the wait.
        const int n = ::recvmmsg(fd_, msgs_.data(), batch_, MSG_WAITFORONE, nullptr);
        if (n <= 0) {
            (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? ++stats_.empty
                                                                                  : ++stats_.errors;
            return 0;
        }
        const u64 t = now_ns();
        ++stats_.wakeups;
        for (int i = 0; i < n; ++i) {
            const mmsghdr& m = msgs_[static_cast<std::size_t>(i)];
            ++stats_.datagrams;
            if ((m.msg_hdr.msg_flags & MSG_TRUNC) != 0) {
                ++stats_.truncated;
                continue;
            }
            on_datagram(std::span<const std::byte>(
                            static_cast<const std::byte*>(m.msg_hdr.msg_iov->iov_base), m.msg_len),
                        t);
        }
        return static_cast<std::size_t>(n);
    }

    [[nodiscard]] const PathStats& stats() const noexcept { return stats_; }

private:
    int                    fd_;
    unsigned               batch_;
    std::vector<std::byte> bufs_;
    std::vector<iovec>     iov_;
    std::vector<mmsghdr>   msgs_;
    PathStats              stats_{};
};

enum class Trigger { Level, Edge };

// epoll_wait for readiness, then recvfrom on a non-blocking socket.
//
// Level-triggered is used the way it usually is: one read per readiness, back
// to epoll_wait, and the kernel reports the socket ready again while data is
// left. Edge-triggered only reports a transition, so it must read until EAGAIN
// or it will never be woken for what is left, and that final EAGAIN is an extra
// system call on every wakeup. For one socket, epoll can only add calls on top
// of a blocking read; what it buys is many sockets, which a single feed does
// not have. The measurement is there to show that, not to assume it.
class EpollPath {
public:
    EpollPath(int fd, u64 timeout_ns, Trigger trigger)
        : fd_(fd), trigger_(trigger), buf_(kDatagramCapacity) {
        net::set_nonblocking(fd, true);
        ep_ = net::Fd(::epoll_create1(EPOLL_CLOEXEC));
        if (!ep_.valid()) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
        epoll_event ev{};
        ev.events = EPOLLIN | (trigger == Trigger::Edge ? EPOLLET : 0u);
        ev.data.fd = fd;
        if (::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
        timeout_ = timespec{static_cast<time_t>(timeout_ns / 1'000'000'000u),
                            static_cast<long>(timeout_ns % 1'000'000'000u)};
    }

    template <class F>
    std::size_t receive(F&& on_datagram) {
        epoll_event ev{};
        ++stats_.syscalls;
        // epoll_pwait2 takes a nanosecond timeout; epoll_wait only milliseconds.
        const int ready = ::epoll_pwait2(ep_.get(), &ev, 1, &timeout_, nullptr);
        if (ready <= 0) {
            ready == 0 || errno == EINTR ? ++stats_.empty : ++stats_.errors;
            return 0;
        }
        ++stats_.wakeups;
        std::size_t got = 0;
        for (;;) {
            ++stats_.syscalls;
            const ssize_t n = ::recv(fd_, buf_.data(), buf_.size(), MSG_TRUNC);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ++stats_.errors;
                }
                break;
            }
            const u64 t = now_ns();
            ++stats_.datagrams;
            ++got;
            if (static_cast<std::size_t>(n) > buf_.size()) {
                ++stats_.truncated;
            } else {
                on_datagram(std::span<const std::byte>(buf_.data(), static_cast<std::size_t>(n)),
                            t);
            }
            if (trigger_ == Trigger::Level) {
                break;
            }
        }
        return got;
    }

    [[nodiscard]] const PathStats& stats() const noexcept { return stats_; }

private:
    int                    fd_;
    Trigger                trigger_;
    net::Fd                ep_;
    timespec               timeout_{};
    std::vector<std::byte> buf_;
    PathStats              stats_{};
};

}  // namespace ttt::rx

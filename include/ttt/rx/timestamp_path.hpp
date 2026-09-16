// A recvmsg path that also reads the kernel's receive timestamps.
//
// SO_TIMESTAMPING gives each datagram up to two stamps: when the kernel's stack
// first saw it (software), and when the NIC received it (hardware, if the NIC
// was told to timestamp every packet with SIOCSHWTSTAMP). Both are
// CLOCK_REALTIME, or the NIC's PTP clock for hardware, so the path reads
// CLOCK_REALTIME right after recvmsg returns and records the two differences on
// that clock:
//
//   stack_to_user  software stamp to recvmsg returning: time in the kernel
//                  network stack, the socket queue, and the wakeup
//   nic_to_user    hardware stamp to recvmsg returning: all of that plus the NIC
//                  and driver. Only comparable across clocks if the system clock
//                  is disciplined to the NIC's clock (phc2sys), and then
//                  phc2sys's residual is the error bar.
//
// These are internal views of where the time goes. The external measurement is
// separate: hardware stamps on the wire, on one clock.
#pragma once

#ifndef __linux__
#error "Linux receive paths"
#endif

#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <system_error>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/measure/histogram.hpp"
#include "ttt/rx/socket_paths.hpp"

namespace ttt::rx {

// Asks the NIC to timestamp every received packet. Returns 0, or the errno
// (EOPNOTSUPP when the device cannot, ERANGE when it cannot do "all").
inline int enable_hw_rx_timestamps(const std::string& iface) noexcept {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return errno;
    }
    hwtstamp_config cfg{};
    cfg.tx_type = HWTSTAMP_TX_OFF;
    cfg.rx_filter = HWTSTAMP_FILTER_ALL;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&cfg);
    const int rc = ::ioctl(fd, SIOCSHWTSTAMP, &ifr);
    const int err = rc == 0 ? 0 : errno;
    ::close(fd);
    if (err == 0 && cfg.rx_filter != HWTSTAMP_FILTER_ALL) {
        return ERANGE;  // the driver substituted a narrower filter
    }
    return err;
}

class TimestampPath {
public:
    TimestampPath(int fd, u64 timeout_ns, bool hardware)
        : fd_(fd), buf_(kDatagramCapacity), hardware_(hardware) {
        set_blocking_with_timeout(fd, timeout_ns);
        int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (hardware) {
            flags |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
        }
        if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) != 0) {
            throw std::system_error(errno, std::generic_category(), "setsockopt(SO_TIMESTAMPING)");
        }
    }

    template <class F>
    std::size_t receive(F&& on_datagram) {
        iovec  iov{buf_.data(), buf_.size()};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control_.data();
        msg.msg_controllen = control_.size();

        ++stats_.syscalls;
        const ssize_t n = ::recvmsg(fd_, &msg, 0);
        if (n < 0) {
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? ++stats_.empty
                                                                        : ++stats_.errors;
            return 0;
        }
        // Read both clocks at once, straight after the call: the monotonic one
        // for the shared loop, the realtime one to compare with kernel stamps.
        const u64 t = now_ns();
        timespec  real{};
        ::clock_gettime(CLOCK_REALTIME, &real);
        ++stats_.wakeups;
        ++stats_.datagrams;
        if ((msg.msg_flags & MSG_TRUNC) != 0) {
            ++stats_.truncated;
            return 1;
        }

        const u64 real_ns = to_ns(real);
        bool      sw = false;
        bool      hw = false;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPING) {
                continue;
            }
            scm_timestamping ts{};
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            if (ts.ts[0].tv_sec != 0 || ts.ts[0].tv_nsec != 0) {
                stack_to_user_.record(diff(real_ns, to_ns(ts.ts[0])));
                sw = true;
            }
            if (ts.ts[2].tv_sec != 0 || ts.ts[2].tv_nsec != 0) {
                nic_to_user_.record(diff(real_ns, to_ns(ts.ts[2])));
                hw = true;
            }
        }
        if (!sw) ++without_sw_;
        if (hardware_ && !hw) ++without_hw_;

        on_datagram(std::span<const std::byte>(buf_.data(), static_cast<std::size_t>(n)), t);
        return 1;
    }

    [[nodiscard]] const PathStats&          stats() const noexcept { return stats_; }
    [[nodiscard]] const measure::Histogram& stack_to_user() const noexcept {
        return stack_to_user_;
    }
    [[nodiscard]] const measure::Histogram& nic_to_user() const noexcept { return nic_to_user_; }
    [[nodiscard]] u64 without_software() const noexcept { return without_sw_; }
    [[nodiscard]] u64 without_hardware() const noexcept { return without_hw_; }

private:
    static u64 to_ns(const timespec& t) noexcept {
        return static_cast<u64>(t.tv_sec) * 1'000'000'000u + static_cast<u64>(t.tv_nsec);
    }
    static itch::i64 diff(u64 a, u64 b) noexcept {
        return a >= b ? static_cast<itch::i64>(a - b) : -static_cast<itch::i64>(b - a);
    }

    int                    fd_;
    std::vector<std::byte> buf_;
    std::array<char, 512>  control_{};
    bool                   hardware_;
    measure::Histogram     stack_to_user_;
    measure::Histogram     nic_to_user_;
    u64                    without_sw_ = 0;
    u64                    without_hw_ = 0;
    PathStats              stats_{};
};

}  // namespace ttt::rx

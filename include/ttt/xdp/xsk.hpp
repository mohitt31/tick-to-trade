// AF_XDP: the XDP program, the UMEM, the four rings and the socket, on raw
// system calls.
//
// Written against the kernel interface directly rather than libxdp's helpers,
// so that every step the zero-copy claim depends on is in this file and can be
// checked: which mode the program attached in, which mode the socket bound in,
// what the kernel says about it afterwards, and where every frame is.
//
// Modes are always forced, never "try the fast one and fall back":
//
//   attach  Native (XDP_FLAGS_DRV_MODE) or Generic (XDP_FLAGS_SKB_MODE). Native
//           on a driver without XDP support fails; it does not quietly become
//           generic. The attached mode is then read back from the kernel.
//   bind    ZeroCopy (XDP_ZEROCOPY) or Copy (XDP_COPY). With neither flag the
//           kernel tries zero-copy and falls back to copy without saying so;
//           that call is never made here. The mode is then read back with
//           getsockopt(XDP_OPTIONS).
//
// Frames. The UMEM is `frames` chunks of `frame_size` bytes. Every frame is
// always in exactly one place: this process's free list, or the kernel (the
// fill ring, a NIC descriptor, or the RX ring). A frame comes back from the
// kernel only through the RX ring, and goes straight back to the fill ring once
// its packet has been handled. So free + with_kernel == frames at all times, and
// that is asserted.
#pragma once

#ifndef __linux__
#error "AF_XDP is Linux only"
#endif

#include <linux/if_xdp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/measure/histogram.hpp"
#include "ttt/net/endpoint.hpp"
#include "ttt/net/socket.hpp"
#include "ttt/rx/socket_paths.hpp"

struct ttt_xdp;  // generated libbpf skeleton
struct bpf_program;

namespace ttt::xdp {

using itch::u16;
using itch::u32;
using itch::u64;

enum class AttachMode { Native, Generic };
enum class BindMode { ZeroCopy, Copy };

[[nodiscard]] const char* to_string(AttachMode m) noexcept;
[[nodiscard]] const char* to_string(BindMode m) noexcept;

struct ProgramCounters {
    u64 redirected = 0;
    u64 no_socket = 0;
    u64 passed = 0;
};

// The XDP program, loaded and attached to one interface for its lifetime.
class Program {
public:
    // rx_timestamps loads the variant that puts the NIC's receive timestamp in
    // front of each redirected frame; native mode only.
    Program(const std::string& iface, AttachMode mode, const net::Endpoint& match,
            bool rx_timestamps = false);
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    [[nodiscard]] int             ifindex() const noexcept { return ifindex_; }
    [[nodiscard]] int             xsks_map_fd() const;
    [[nodiscard]] AttachMode      requested() const noexcept { return mode_; }
    // What the kernel reports is attached now: "native", "generic", "offload",
    // "multi" or "none".
    [[nodiscard]] std::string     attached_mode() const;
    [[nodiscard]] ProgramCounters counters() const;
    [[nodiscard]] bool            rx_timestamps() const noexcept { return rx_timestamps_; }

private:
    ttt_xdp*     skel_ = nullptr;
    bpf_program* prog_ = nullptr;
    int          ifindex_ = 0;
    AttachMode   mode_;
    bool         rx_timestamps_ = false;
};

struct SocketConfig {
    u32      queue = 0;
    BindMode bind = BindMode::Copy;
    u32      frames = 4096;      // power of two
    u32      frame_size = 4096;  // power of two, at least a page
    u32      ring_size = 2048;   // power of two, for all four rings
    bool     need_wakeup = true;
    bool     hugepages = false;  // back the UMEM with 2 MiB pages
};

class Socket {
public:
    Socket(const Program& prog, const SocketConfig& cfg);
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int            fd() const noexcept { return fd_.get(); }
    [[nodiscard]] bool           zerocopy() const;  // XDP_OPTIONS, read from the kernel
    [[nodiscard]] xdp_statistics statistics() const;
    [[nodiscard]] int umem_numa_node() const;  // node of the UMEM's first page, -1 if unknown
    [[nodiscard]] const SocketConfig& config() const noexcept { return cfg_; }

    // Frame accounting, for the conservation invariant.
    [[nodiscard]] u32 free_frames() const noexcept { return static_cast<u32>(free_.size()); }
    [[nodiscard]] u64 with_kernel() const noexcept { return given_ - taken_; }

    // Takes up to max received frames: calls on_frame(bytes) for each, then
    // returns every frame to the fill ring. Returns how many.
    template <class F>
    u32 take(u32 max, F&& on_frame) {
        const u32 prod = rx_.producer.load();
        u32       cons = *rx_.consumer;
        u32       n = prod - cons;
        if (n == 0) {
            return 0;
        }
        n = n < max ? n : max;
        for (u32 i = 0; i < n; ++i) {
            const xdp_desc& d = static_cast<xdp_desc*>(rx_.descs)[cons & rx_.mask];
            ++cons;
            on_frame(std::span<const std::byte>(static_cast<std::byte*>(umem_) + d.addr, d.len));
            // Aligned mode: the frame starts at the chunk boundary below addr.
            free_.push_back(d.addr & ~static_cast<u64>(cfg_.frame_size - 1));
        }
        *rx_.consumer = cons;
        taken_ += n;
        refill();
        ITCH_INVARIANT_MSG(free_.size() + with_kernel() == cfg_.frames,
                           "a UMEM frame is unaccounted for");
        return n;
    }

    // Where a pointer into the UMEM sits inside its frame. In aligned mode a
    // packet starts after the kernel's headroom, and XDP metadata sits just
    // before the packet, inside that headroom.
    [[nodiscard]] u64 frame_offset(const std::byte* p) const noexcept {
        return static_cast<u64>(p - static_cast<const std::byte*>(umem_)) & (cfg_.frame_size - 1);
    }

    // The fill ring asks to be kicked when the kernel ran out of frames to use.
    [[nodiscard]] bool fill_needs_wakeup() const noexcept {
        return cfg_.need_wakeup && (fill_.flags.load() & XDP_RING_NEED_WAKEUP) != 0;
    }

private:
    struct Ring {
        struct Counter {
            u32*              p = nullptr;
            [[nodiscard]] u32 load() const noexcept { return *p; }
        };
        Counter     producer;
        u32*        consumer = nullptr;
        Counter     flags;
        void*       descs = nullptr;
        u32         mask = 0;
        void*       map = nullptr;
        std::size_t map_len = 0;
    };

    void map_ring(Ring& r, const xdp_ring_offset& off, std::size_t entry, off_t pgoff);
    void refill();

    const Program&   prog_;
    SocketConfig     cfg_;
    net::Fd          fd_;
    void*            umem_ = nullptr;
    std::size_t      umem_len_ = 0;
    Ring             fill_;
    Ring             comp_;
    Ring             rx_;
    std::vector<u64> free_;
    u64              given_ = 0;  // frames put on the fill ring
    u64              taken_ = 0;  // frames taken back from the RX ring
};

// A receive path over one AF_XDP socket, the same shape as the socket paths.
//
// Waiting: by default poll() on the socket. With busy_poll, the path never
// sleeps: an empty RX ring is answered with a non-blocking recvfrom on the
// socket, which is what drives the driver's NAPI poll from this thread when
// SO_PREFER_BUSY_POLL and SO_BUSY_POLL are set, until the timeout.
class XdpPath {
public:
    static constexpr u32 kBatch = 64;

    XdpPath(Socket& sock, const net::Endpoint& match, u64 timeout_ns, bool busy_poll,
            bool rx_timestamps = false);

    template <class F>
    std::size_t receive(F&& on_datagram) {
        const u64 deadline = now_ns() + timeout_ns_;
        for (;;) {
            u64         t = 0;
            u64         real = 0;
            std::size_t got = 0;
            const u32   n = sock_.take(kBatch, [&](std::span<const std::byte> frame) {
                if (t == 0) {
                    t = now_ns();
                    if (rx_timestamps_) {
                        timespec r{};
                        ::clock_gettime(CLOCK_REALTIME, &r);
                        real = static_cast<u64>(r.tv_sec) * 1'000'000'000u +
                               static_cast<u64>(r.tv_nsec);
                    }
                }
                if (rx_timestamps_) {
                    read_meta(frame, real);
                }
                ++stats_.datagrams;
                ++got;
                const auto payload = udp_payload(frame);
                if (payload.empty()) {
                    ++stats_.errors;  // not the feed: the program should never send it
                    return;
                }
                on_datagram(payload, t);
            });
            if (n != 0) {
                ++stats_.wakeups;
                return got;
            }
            if (busy_poll_) {
                ++stats_.syscalls;
                (void)::recvfrom(sock_.fd(), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
                if (now_ns() >= deadline) {
                    ++stats_.empty;
                    return 0;
                }
                continue;
            }
            if (sock_.fill_needs_wakeup()) {
                ++stats_.syscalls;
                (void)::recvfrom(sock_.fd(), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
            }
            const u64 now = now_ns();
            if (now >= deadline) {
                ++stats_.empty;
                return 0;
            }
            pollfd p{sock_.fd(), POLLIN, 0};
            ++stats_.syscalls;
            const int ms = static_cast<int>((deadline - now + 999'999) / 1'000'000);
            if (::poll(&p, 1, ms) <= 0) {
                ++stats_.empty;
                return 0;
            }
        }
    }

    [[nodiscard]] const rx::PathStats& stats() const noexcept { return stats_; }

    // With rx_timestamps: the NIC's receive stamp to the frame being taken, on
    // CLOCK_REALTIME against the NIC's clock (phc2sys caveat as for sockets).
    [[nodiscard]] const measure::Histogram& nic_to_user() const noexcept { return nic_to_user_; }
    [[nodiscard]] u64                       meta_missing() const noexcept { return meta_missing_; }
    [[nodiscard]] u64                       unstamped() const noexcept { return unstamped_; }
    [[nodiscard]] itch::i32                 last_kfunc_rc() const noexcept { return last_rc_; }

private:
    // The UDP payload of an Ethernet/IPv4/UDP frame to the matched endpoint, or
    // empty.
    [[nodiscard]] std::span<const std::byte> udp_payload(
        std::span<const std::byte> f) const noexcept {
        if (f.size() < 14 + 20 + 8 || itch::load_be<u16>(f.data() + 12) != 0x0800) {
            return {};
        }
        const std::size_t ihl = static_cast<std::size_t>(static_cast<u32>(f[14]) & 0x0F) * 4;
        if (ihl < 20 || f.size() < 14 + ihl + 8 || static_cast<u32>(f[14 + 9]) != 17) {
            return {};
        }
        const std::size_t udp = 14 + ihl;
        if (itch::load_be<u16>(f.data() + udp + 2) != match_.port) {
            return {};
        }
        const std::size_t len = itch::load_be<u16>(f.data() + udp + 4);
        if (len < 8 || udp + len > f.size()) {
            return {};
        }
        return f.subspan(udp + 8, len - 8);
    }

    // The metadata the timestamping program puts in front of a frame.
    struct RxMeta {
        u64       timestamp;
        itch::i32 rc;
        u32       magic;
    };
    static constexpr u32 kMetaMagic = 0x54544d31;

    void read_meta(std::span<const std::byte> frame, u64 real_ns) noexcept {
        if (sock_.frame_offset(frame.data()) < sizeof(RxMeta)) {
            ++meta_missing_;
            return;
        }
        RxMeta m{};
        std::memcpy(&m, frame.data() - sizeof(RxMeta), sizeof(RxMeta));
        if (m.magic != kMetaMagic) {
            ++meta_missing_;  // metadata did not survive to the socket
        } else if (m.rc != 0 || m.timestamp == 0) {
            ++unstamped_;
            last_rc_ = m.rc;
        } else {
            nic_to_user_.record(real_ns >= m.timestamp
                                    ? static_cast<itch::i64>(real_ns - m.timestamp)
                                    : -static_cast<itch::i64>(m.timestamp - real_ns));
        }
    }

    Socket&            sock_;
    net::Endpoint      match_;
    u64                timeout_ns_;
    bool               busy_poll_;
    bool               rx_timestamps_ = false;
    measure::Histogram nic_to_user_;
    u64                meta_missing_ = 0;
    u64                unstamped_ = 0;
    itch::i32          last_rc_ = 0;
    rx::PathStats      stats_{};
};

}  // namespace ttt::xdp

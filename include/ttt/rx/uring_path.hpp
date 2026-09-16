// io_uring receive path: one multishot recvmsg with a provided buffer ring.
//
// A single submission keeps producing a completion per datagram, and the kernel
// picks each datagram's buffer from a ring of buffers registered up front, so
// the steady state has no per-packet submission and no per-packet buffer
// setup. What it does not remove is the rest of the kernel network stack or the
// copy into the buffer: that is still a socket receive. Three ways of running
// the ring:
//
//   Plain     io_uring_enter to wait for completions, task work runs on return
//   Sqpoll    a kernel thread polls the submission queue; completions are still
//             waited for
//   Defer     DEFER_TASKRUN with SINGLE_ISSUER: completion work runs only when
//             this thread enters the kernel, never interrupting it
//
// Zero-copy receive (zcrx) is not here: it needs a NIC with header/data split,
// which the target hardware does not have.
#pragma once

#ifndef __linux__
#error "Linux receive paths"
#endif

#include <liburing.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <system_error>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/rx/socket_paths.hpp"

namespace ttt::rx {

enum class UringMode { Plain, Sqpoll, Defer };

class UringPath {
public:
    static constexpr unsigned kBuffers = 1024;  // power of two
    static constexpr int      kGroup = 1;

    UringPath(int fd, u64 timeout_ns, UringMode mode);
    ~UringPath();
    UringPath(const UringPath&) = delete;
    UringPath& operator=(const UringPath&) = delete;

    template <class F>
    std::size_t receive(F&& on_datagram) {
        if (!armed_) {
            arm();
        }
        io_uring_cqe*     cqe = nullptr;
        __kernel_timespec ts{static_cast<long long>(timeout_ns_ / 1'000'000'000u),
                             static_cast<long long>(timeout_ns_ % 1'000'000'000u)};
        ++stats_.syscalls;
        const int rc = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) {
            ++stats_.errors;
            return 0;
        }
        const u64 t = now_ns();

        std::size_t got = 0;
        unsigned    seen = 0;
        unsigned    head = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++seen;
            if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
                armed_ = false;  // multishot ended; re-arm on the next call
            }
            if (cqe->res < 0) {
                cqe->res == -ENOBUFS ? ++stats_.truncated : ++stats_.errors;
                continue;
            }
            if ((cqe->flags & IORING_CQE_F_BUFFER) == 0) {
                ++stats_.errors;
                continue;
            }
            const unsigned        bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            auto*                 base = static_cast<void*>(bufs_.data() + bid * kDatagramCapacity);
            io_uring_recvmsg_out* out = io_uring_recvmsg_validate(base, cqe->res, &tmpl_);
            ++stats_.datagrams;
            ++got;
            if (out == nullptr || (out->flags & MSG_TRUNC) != 0) {
                ++stats_.truncated;
            } else {
                const auto* payload =
                    static_cast<const std::byte*>(io_uring_recvmsg_payload(out, &tmpl_));
                const auto len = io_uring_recvmsg_payload_length(out, cqe->res, &tmpl_);
                on_datagram(std::span<const std::byte>(payload, len), t);
            }
            // The buffer goes straight back to the ring.
            io_uring_buf_ring_add(br_, base, kDatagramCapacity, static_cast<unsigned short>(bid),
                                  io_uring_buf_ring_mask(kBuffers), static_cast<int>(returned_++));
        }
        if (returned_ != 0) {
            io_uring_buf_ring_advance(br_, static_cast<int>(returned_));
            returned_ = 0;
        }
        io_uring_cq_advance(&ring_, seen);
        got != 0 ? ++stats_.wakeups : ++stats_.empty;
        return got;
    }

    [[nodiscard]] const PathStats& stats() const noexcept { return stats_; }

private:
    void arm();

    int                    fd_;
    u64                    timeout_ns_;
    io_uring               ring_{};
    io_uring_buf_ring*     br_ = nullptr;
    std::vector<std::byte> bufs_;
    msghdr                 tmpl_{};
    bool                   armed_ = false;
    unsigned               returned_ = 0;
    PathStats              stats_{};
};

}  // namespace ttt::rx

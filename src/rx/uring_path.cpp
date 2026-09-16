#include "ttt/rx/uring_path.hpp"

#include <stdexcept>

namespace ttt::rx {

UringPath::UringPath(int fd, u64 timeout_ns, UringMode mode)
    : fd_(fd), timeout_ns_(timeout_ns), bufs_(kBuffers * kDatagramCapacity) {
    net::set_nonblocking(fd, true);

    io_uring_params p{};
    switch (mode) {
        case UringMode::Plain: break;
        case UringMode::Sqpoll:
            p.flags = IORING_SETUP_SQPOLL;
            p.sq_thread_idle = 1000;  // ms before the poll thread sleeps
            break;
        case UringMode::Defer:
            p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
            break;
    }
    if (const int rc = io_uring_queue_init_params(256, &ring_, &p); rc < 0) {
        throw std::system_error(-rc, std::generic_category(), "io_uring_queue_init_params");
    }

    int ret = 0;
    br_ = io_uring_setup_buf_ring(&ring_, kBuffers, kGroup, 0, &ret);
    if (br_ == nullptr) {
        io_uring_queue_exit(&ring_);
        throw std::system_error(-ret, std::generic_category(), "io_uring_setup_buf_ring");
    }
    const int mask = io_uring_buf_ring_mask(kBuffers);
    for (unsigned i = 0; i < kBuffers; ++i) {
        io_uring_buf_ring_add(br_, bufs_.data() + i * kDatagramCapacity, kDatagramCapacity,
                              static_cast<unsigned short>(i), mask, static_cast<int>(i));
    }
    io_uring_buf_ring_advance(br_, static_cast<int>(kBuffers));

    // No address and no control data: the buffer holds the recvmsg_out header
    // and then the payload.
    tmpl_.msg_namelen = 0;
    tmpl_.msg_controllen = 0;
}

UringPath::~UringPath() {
    if (br_ != nullptr) {
        io_uring_free_buf_ring(&ring_, br_, kBuffers, kGroup);
    }
    io_uring_queue_exit(&ring_);
}

void UringPath::arm() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        throw std::runtime_error("io_uring submission queue full");
    }
    io_uring_prep_recvmsg_multishot(sqe, fd_, &tmpl_, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kGroup;
    armed_ = true;
}

}  // namespace ttt::rx

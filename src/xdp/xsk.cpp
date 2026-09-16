#include "ttt/xdp/xsk.hpp"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <system_error>

#include "ttt_xdp.skel.h"

namespace ttt::xdp {

namespace {

[[noreturn]] void fail_errno(int err, const std::string& what) {
    throw std::system_error(err, std::generic_category(), what);
}

u32 attach_flags(AttachMode m) noexcept {
    return m == AttachMode::Native ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
}

bool power_of_two(u32 v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

}  // namespace

const char* to_string(AttachMode m) noexcept {
    return m == AttachMode::Native ? "native" : "generic";
}
const char* to_string(BindMode m) noexcept { return m == BindMode::ZeroCopy ? "zerocopy" : "copy"; }

// --- the program ------------------------------------------------------------

Program::Program(const std::string& iface, AttachMode mode, const net::Endpoint& match)
    : mode_(mode) {
    ifindex_ = static_cast<int>(::if_nametoindex(iface.c_str()));
    if (ifindex_ == 0) {
        fail_errno(errno, "if_nametoindex(" + iface + ")");
    }
    skel_ = ttt_xdp__open_and_load();
    if (skel_ == nullptr) {
        fail_errno(errno, "loading the XDP program");
    }

    struct Config {
        u32 dst_ip;
        u16 dst_port;
        u16 pad;
    } cfg{};
    std::memcpy(&cfg.dst_ip, match.ip.data(), 4);
    cfg.dst_port = static_cast<u16>((match.port >> 8) | (match.port << 8));
    const u32 zero = 0;
    if (bpf_map__update_elem(skel_->maps.ttt_config, &zero, sizeof(zero), &cfg, sizeof(cfg),
                             BPF_ANY) != 0) {
        const int e = errno;
        ttt_xdp__destroy(skel_);
        fail_errno(e, "writing the XDP program's config");
    }

    // Forced mode, and never replacing a program someone else attached.
    const int prog_fd = bpf_program__fd(skel_->progs.ttt_xdp_redirect);
    const int rc = bpf_xdp_attach(ifindex_, prog_fd,
                                  attach_flags(mode) | XDP_FLAGS_UPDATE_IF_NOEXIST, nullptr);
    if (rc != 0) {
        ttt_xdp__destroy(skel_);
        fail_errno(-rc, std::string("attaching XDP in ") + to_string(mode) + " mode to " + iface);
    }
}

Program::~Program() {
    (void)bpf_xdp_detach(ifindex_, attach_flags(mode_), nullptr);
    ttt_xdp__destroy(skel_);
}

int Program::xsks_map_fd() const { return bpf_map__fd(skel_->maps.ttt_xsks); }

std::string Program::attached_mode() const {
    LIBBPF_OPTS(bpf_xdp_query_opts, q);
    if (bpf_xdp_query(ifindex_, 0, &q) != 0) {
        return "unknown";
    }
    switch (q.attach_mode) {
        case XDP_ATTACHED_NONE: return "none";
        case XDP_ATTACHED_DRV: return "native";
        case XDP_ATTACHED_SKB: return "generic";
        case XDP_ATTACHED_HW: return "offload";
        case XDP_ATTACHED_MULTI: return "multi";
        default: return "unknown";
    }
}

ProgramCounters Program::counters() const {
    const int        cpus = libbpf_num_possible_cpus();
    std::vector<u64> per_cpu(static_cast<std::size_t>(cpus > 0 ? cpus : 1));
    ProgramCounters  out;
    u64*             fields[3] = {&out.redirected, &out.no_socket, &out.passed};
    for (u32 k = 0; k < 3; ++k) {
        if (bpf_map__lookup_elem(skel_->maps.ttt_counters, &k, sizeof(k), per_cpu.data(),
                                 per_cpu.size() * sizeof(u64), 0) == 0) {
            for (u64 v : per_cpu) {
                *fields[k] += v;
            }
        }
    }
    return out;
}

// --- the socket -------------------------------------------------------------

Socket::Socket(const Program& prog, const SocketConfig& cfg) : prog_(prog), cfg_(cfg) {
    if (!power_of_two(cfg.frames) || !power_of_two(cfg.frame_size) ||
        !power_of_two(cfg.ring_size)) {
        throw std::invalid_argument("frames, frame size and ring size must be powers of two");
    }

    fd_ = net::Fd(::socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0));
    if (!fd_.valid()) {
        fail_errno(errno, "socket(AF_XDP)");
    }

    // The UMEM: one anonymous mapping, faulted in and locked now, so no packet
    // ever takes a page fault on it.
    umem_len_ = static_cast<std::size_t>(cfg.frames) * cfg.frame_size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
    if (cfg.hugepages) {
        flags |= MAP_HUGETLB;
    }
    umem_ = ::mmap(nullptr, umem_len_, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (umem_ == MAP_FAILED) {
        umem_ = nullptr;
        fail_errno(errno, cfg.hugepages ? "mmap(UMEM, hugepages)" : "mmap(UMEM)");
    }
    (void)::mlock(umem_, umem_len_);

    xdp_umem_reg reg{};
    reg.addr = reinterpret_cast<u64>(umem_);
    reg.len = umem_len_;
    reg.chunk_size = cfg.frame_size;
    reg.headroom = 0;
    if (::setsockopt(fd_.get(), SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)) != 0) {
        fail_errno(errno, "setsockopt(XDP_UMEM_REG)");
    }
    // The fill and completion rings belong to the UMEM and are required even
    // for a socket that never transmits.
    const int ring = static_cast<int>(cfg.ring_size);
    if (::setsockopt(fd_.get(), SOL_XDP, XDP_UMEM_FILL_RING, &ring, sizeof(ring)) != 0 ||
        ::setsockopt(fd_.get(), SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring, sizeof(ring)) != 0 ||
        ::setsockopt(fd_.get(), SOL_XDP, XDP_RX_RING, &ring, sizeof(ring)) != 0) {
        fail_errno(errno, "setsockopt(ring sizes)");
    }

    xdp_mmap_offsets off{};
    socklen_t        len = sizeof(off);
    if (::getsockopt(fd_.get(), SOL_XDP, XDP_MMAP_OFFSETS, &off, &len) != 0) {
        fail_errno(errno, "getsockopt(XDP_MMAP_OFFSETS)");
    }
    map_ring(fill_, off.fr, sizeof(u64), XDP_UMEM_PGOFF_FILL_RING);
    map_ring(comp_, off.cr, sizeof(u64), XDP_UMEM_PGOFF_COMPLETION_RING);
    map_ring(rx_, off.rx, sizeof(xdp_desc), XDP_PGOFF_RX_RING);

    free_.reserve(cfg.frames);
    for (u32 i = cfg.frames; i > 0; --i) {
        free_.push_back(static_cast<u64>(i - 1) * cfg.frame_size);
    }
    refill();

    sockaddr_xdp sa{};
    sa.sxdp_family = AF_XDP;
    sa.sxdp_ifindex = static_cast<u32>(prog.ifindex());
    sa.sxdp_queue_id = cfg.queue;
    sa.sxdp_flags = static_cast<u16>((cfg.bind == BindMode::ZeroCopy ? XDP_ZEROCOPY : XDP_COPY) |
                                     (cfg.need_wakeup ? XDP_USE_NEED_WAKEUP : 0));
    if (::bind(fd_.get(), reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        fail_errno(errno, std::string("bind(AF_XDP, ") + to_string(cfg.bind) + ", queue " +
                              std::to_string(cfg.queue) + ")");
    }
    // Belt and braces: a forced bind cannot silently change mode, but the claim
    // rests on it, so the kernel is asked.
    if (zerocopy() != (cfg.bind == BindMode::ZeroCopy)) {
        throw std::runtime_error("AF_XDP socket is bound in a different mode than requested");
    }

    const u32 q = cfg.queue;
    const int xsk = fd_.get();
    if (bpf_map_update_elem(prog.xsks_map_fd(), &q, &xsk, BPF_ANY) != 0) {
        fail_errno(errno, "inserting the socket into the XSKMAP");
    }
}

Socket::~Socket() {
    const u32 q = cfg_.queue;
    (void)bpf_map_delete_elem(prog_.xsks_map_fd(), &q);
    for (Ring* r : {&fill_, &comp_, &rx_}) {
        if (r->map != nullptr) {
            ::munmap(r->map, r->map_len);
        }
    }
    fd_.reset();
    if (umem_ != nullptr) {
        ::munmap(umem_, umem_len_);
    }
}

void Socket::map_ring(Ring& r, const xdp_ring_offset& off, std::size_t entry, off_t pgoff) {
    r.map_len = off.desc + cfg_.ring_size * entry;
    r.map = ::mmap(nullptr, r.map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_.get(),
                   pgoff);
    if (r.map == MAP_FAILED) {
        r.map = nullptr;
        fail_errno(errno, "mmap(AF_XDP ring)");
    }
    auto* base = static_cast<char*>(r.map);
    r.producer.p = reinterpret_cast<u32*>(base + off.producer);
    r.consumer = reinterpret_cast<u32*>(base + off.consumer);
    r.flags.p = reinterpret_cast<u32*>(base + off.flags);
    r.descs = base + off.desc;
    r.mask = cfg_.ring_size - 1;
}

// Moves free frames onto the fill ring, as many as it has room for.
void Socket::refill() {
    const u32 cons = *fill_.consumer;
    u32       prod = fill_.producer.load();
    const u32 room = cfg_.ring_size - (prod - cons);
    u32       n = room < free_.size() ? room : static_cast<u32>(free_.size());
    auto*     slots = static_cast<u64*>(fill_.descs);
    for (u32 i = 0; i < n; ++i) {
        slots[prod & fill_.mask] = free_.back();
        free_.pop_back();
        ++prod;
    }
    *fill_.producer.p = prod;
    given_ += n;
}

bool Socket::zerocopy() const {
    xdp_options o{};
    socklen_t   len = sizeof(o);
    if (::getsockopt(fd_.get(), SOL_XDP, XDP_OPTIONS, &o, &len) != 0) {
        fail_errno(errno, "getsockopt(XDP_OPTIONS)");
    }
    return (o.flags & XDP_OPTIONS_ZEROCOPY) != 0;
}

xdp_statistics Socket::statistics() const {
    xdp_statistics s{};
    socklen_t      len = sizeof(s);
    if (::getsockopt(fd_.get(), SOL_XDP, XDP_STATISTICS, &s, &len) != 0) {
        fail_errno(errno, "getsockopt(XDP_STATISTICS)");
    }
    return s;
}

int Socket::umem_numa_node() const {
    // get_mempolicy with MPOL_F_NODE | MPOL_F_ADDR reports the node the page at
    // addr is actually on. The page is already faulted in, by MAP_POPULATE.
    int        node = -1;
    const long rc =
        ::syscall(SYS_get_mempolicy, &node, nullptr, 0, umem_, 3 /* MPOL_F_NODE | MPOL_F_ADDR */);
    return rc == 0 ? node : -1;
}

// --- the path ---------------------------------------------------------------

XdpPath::XdpPath(Socket& sock, const net::Endpoint& match, u64 timeout_ns, bool busy_poll)
    : sock_(sock), match_(match), timeout_ns_(timeout_ns), busy_poll_(busy_poll) {}

}  // namespace ttt::xdp

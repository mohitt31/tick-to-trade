// Measures one receive path on one feed. Linux only.
//
// Usage:
//   ttt_rxbench --path PATH [options]
//
//   --path recvfrom | recvmmsg:BATCH | epoll-lt | epoll-et | uring | uring-sqpoll | uring-defer
//   --feed A.B.C.D:PORT        bind; a multicast group is joined (default 239.1.1.1:30001)
//   --iface A.B.C.D            interface address for the join (default 127.0.0.1)
//   --session NAME  --symbol SYM
//   --rewind A.B.C.D:PORT      ask here for retransmissions (default: never ask)
//   --rcvbuf BYTES             SO_RCVBUF (default 8 MiB)
//   --cpu N                    pin the receiving thread to CPU N
//   --fifo PRIORITY            run under SCHED_FIFO at this priority
//   --mlock                    mlockall(MCL_CURRENT | MCL_FUTURE) before receiving
//   --busy-poll-us N           SO_BUSY_POLL on the socket
//   --prefer-busy-poll         SO_PREFER_BUSY_POLL on the socket
//   --latency-out PREFIX       write PREFIX-<histogram>.hgrm, each headed by the manifest
//   --manifest-iface NAME      describe this interface in the manifest
//   --idle-timeout-ms N        (default 5000)
//
// The sender must be ttt_feedd --trailer on the same host, so both read one clock.
// Every knob set here is also read back from the kernel into the manifest.

#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ttt/measure/manifest.hpp"
#include "ttt/rx/bench_receiver.hpp"
#include "ttt/rx/socket_paths.hpp"
#include "ttt/rx/uring_path.hpp"

using namespace ttt;
using itch::u64;

namespace {

std::atomic<bool> g_stop{false};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "ttt_rxbench: %s\nsee the comment at the top of apps/rxbench.cpp\n",
                 why.c_str());
    std::exit(2);
}

u64 num(const std::string& s) {
    char*     end = nullptr;
    const u64 v = std::strtoull(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0') usage("bad number: " + s);
    return v;
}

net::Endpoint ep(const std::string& s) {
    net::Endpoint e;
    if (!net::parse_endpoint(s, e)) usage("bad endpoint: " + s);
    return e;
}

void setsock(int fd, int opt, int value, const char* what) {
    if (::setsockopt(fd, SOL_SOCKET, opt, &value, sizeof(value)) != 0) {
        std::perror(what);
        std::exit(1);
    }
}

// The kernel's own UDP counters, from /proc/net/snmp: the independent check on
// any loss the sequence numbers show.
struct UdpCounters {
    u64 in_datagrams = 0;
    u64 in_errors = 0;
    u64 rcvbuf_errors = 0;
};

UdpCounters udp_counters() {
    std::FILE* f = std::fopen("/proc/net/snmp", "r");
    if (f == nullptr) return {};
    char                     line[1024];
    std::vector<std::string> names;
    UdpCounters              c;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "Udp: ", 5) != 0) continue;
        std::vector<std::string> fields;
        for (char* tok = std::strtok(line + 5, " \n"); tok != nullptr;
             tok = std::strtok(nullptr, " \n")) {
            fields.emplace_back(tok);
        }
        if (names.empty()) {
            names = fields;
            continue;
        }
        for (std::size_t i = 0; i < names.size() && i < fields.size(); ++i) {
            const u64 v = std::strtoull(fields[i].c_str(), nullptr, 10);
            if (names[i] == "InDatagrams") c.in_datagrams = v;
            if (names[i] == "InErrors") c.in_errors = v;
            if (names[i] == "RcvbufErrors") c.rcvbuf_errors = v;
        }
        break;
    }
    std::fclose(f);
    return c;
}

template <class Path>
int report(Path& path, const rx::BenchConfig& bc, const std::string& path_name,
           const std::string& latency_out, const std::string& manifest_iface) {
    const UdpCounters     before = udp_counters();
    const rx::BenchResult r = rx::run_bench(path, bc, g_stop);
    const UdpCounters     after = udp_counters();
    const auto&           l = r.rx.line;
    std::printf("path %s\nstopped: %s\n", path_name.c_str(), r.rx.stopped_because.c_str());
    std::printf("applied %" PRIu64 "  missing %" PRIu64 "  unapplied %" PRIu64
                "  duplicates %" PRIu64 "  late %" PRIu64 "  decode errors %" PRIu64 "\n",
                r.rx.applied, r.missing, r.unapplied, r.sequence.duplicate_packets,
                r.sequence.late_packets, l.decode_errors);
    std::printf("kernel udp (system-wide, during the run): in %" PRIu64 "  errors %" PRIu64
                "  rcvbuf errors %" PRIu64 "\n",
                after.in_datagrams - before.in_datagrams, after.in_errors - before.in_errors,
                after.rcvbuf_errors - before.rcvbuf_errors);
    std::printf("syscalls %" PRIu64 "  wakeups %" PRIu64 "  empty %" PRIu64 "  datagrams %" PRIu64
                "  truncated %" PRIu64 "  errors %" PRIu64 "\n",
                r.path.syscalls, r.path.wakeups, r.path.empty, r.path.datagrams, r.path.truncated,
                r.path.errors);
    std::printf("batch             %s\n", r.batch.summary().c_str());

    const auto&             lat = *r.rx.latency;
    const measure::Manifest manifest = measure::collect_manifest(
        measure::ManifestOptions{manifest_iface, "ttt_rxbench " + path_name});
    const std::pair<const char*, const measure::Histogram*> hists[] = {
        {"intended_to_recv", &lat.intended_to_recv},
        {"sent_to_recv", &lat.sent_to_recv},
        {"sender_lag", &lat.sender_lag},
        {"handler", &lat.handler},
    };
    for (const auto& [name, h] : hists) {
        std::printf("%-17s %s\n", name, h->summary().c_str());
        if (latency_out.empty()) continue;
        const std::string path_out = latency_out + "-" + name + ".hgrm";
        std::FILE*        f = std::fopen(path_out.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "ttt_rxbench: cannot write %s\n", path_out.c_str());
            return 1;
        }
        measure::write_manifest(f, manifest);
        std::fprintf(f,
                     "# path_stats: syscalls %" PRIu64 " wakeups %" PRIu64 " datagrams %" PRIu64
                     " lost_messages %" PRIu64 "\n",
                     r.path.syscalls, r.path.wakeups, r.path.datagrams, r.missing);
        std::fprintf(f, "# histogram: %s (values in microseconds)\n", name);
        h->write_hgrm(f);
        std::fclose(f);
    }
    return r.rx.ended && !r.rx.stale && r.missing == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) try {
    std::string     path_name;
    net::Endpoint   feed = ep("239.1.1.1:30001");
    net::Ipv4       iface = net::kLoopback;
    rx::BenchConfig bc;
    bc.line.session = mold::Session("TTTLIVE");
    int         rcvbuf = 8 << 20;
    int         cpu = -1;
    int         fifo = 0;
    bool        mlock = false;
    int         busy_poll_us = 0;
    bool        prefer_busy_poll = false;
    std::string latency_out;
    std::string manifest_iface;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--path")
            path_name = val();
        else if (a == "--feed")
            feed = ep(val());
        else if (a == "--iface")
            iface = ep(val() + ":0").ip;
        else if (a == "--session") {
            const std::string v = val();
            if (v.size() > mold::kSessionSize) usage("session longer than 10 chars");
            bc.line.session = mold::Session(v);
        } else if (a == "--symbol")
            bc.symbol = val();
        else if (a == "--rewind")
            bc.rewind_server = ep(val());
        else if (a == "--rcvbuf")
            rcvbuf = static_cast<int>(num(val()));
        else if (a == "--cpu")
            cpu = static_cast<int>(num(val()));
        else if (a == "--fifo")
            fifo = static_cast<int>(num(val()));
        else if (a == "--mlock")
            mlock = true;
        else if (a == "--busy-poll-us")
            busy_poll_us = static_cast<int>(num(val()));
        else if (a == "--prefer-busy-poll")
            prefer_busy_poll = true;
        else if (a == "--latency-out")
            latency_out = val();
        else if (a == "--manifest-iface")
            manifest_iface = val();
        else if (a == "--idle-timeout-ms")
            bc.idle_timeout_ns = num(val()) * 1'000'000;
        else
            usage("unknown option " + a);
    }
    if (path_name.empty()) usage("--path is required");

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<std::size_t>(cpu), &set);
        if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
            std::perror("sched_setaffinity");
            return 1;
        }
    }
    if (fifo > 0) {
        sched_param sp{};
        sp.sched_priority = fifo;
        if (::sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            std::perror("sched_setscheduler(SCHED_FIFO)");
            return 1;
        }
    }
    if (mlock && ::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::perror("mlockall");
        return 1;
    }

    net::UdpOptions o;
    o.bind = feed;
    o.iface = iface;
    o.join = feed.multicast();
    o.rcvbuf = rcvbuf;
    net::Fd fd = net::udp_socket(o);
    if (busy_poll_us > 0) setsock(fd.get(), SO_BUSY_POLL, busy_poll_us, "SO_BUSY_POLL");
    if (prefer_busy_poll) setsock(fd.get(), SO_PREFER_BUSY_POLL, 1, "SO_PREFER_BUSY_POLL");
    // The kernel reports twice what it grants, for its own bookkeeping.
    std::printf("rcvbuf requested %d  granted %d\n", rcvbuf, net::effective_rcvbuf(fd.get()) / 2);

    std::signal(SIGINT, [](int) { g_stop = true; });
    const u64 timeout = 10'000'000;

    if (path_name == "recvfrom") {
        rx::RecvfromPath p(fd.get(), timeout);
        return report(p, bc, path_name, latency_out, manifest_iface);
    }
    if (path_name.rfind("recvmmsg:", 0) == 0) {
        const u64 batch = num(path_name.substr(9));
        if (batch == 0 || batch > 1024) usage("recvmmsg batch must be in 1..1024");
        rx::RecvmmsgPath p(fd.get(), timeout, static_cast<unsigned>(batch));
        return report(p, bc, path_name, latency_out, manifest_iface);
    }
    if (path_name == "epoll-lt" || path_name == "epoll-et") {
        rx::EpollPath p(fd.get(), timeout,
                        path_name == "epoll-lt" ? rx::Trigger::Level : rx::Trigger::Edge);
        return report(p, bc, path_name, latency_out, manifest_iface);
    }
    if (path_name == "uring" || path_name == "uring-sqpoll" || path_name == "uring-defer") {
        const rx::UringMode mode = path_name == "uring"          ? rx::UringMode::Plain
                                   : path_name == "uring-sqpoll" ? rx::UringMode::Sqpoll
                                                                 : rx::UringMode::Defer;
        rx::UringPath       p(fd.get(), timeout, mode);
        return report(p, bc, path_name, latency_out, manifest_iface);
    }
    usage("unknown path " + path_name);
} catch (const std::exception& ex) {
    std::fprintf(stderr, "ttt_rxbench: %s\n", ex.what());
    return 1;
}

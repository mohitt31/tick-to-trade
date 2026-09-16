// Gate 0: does this interface really do what the AF_XDP numbers will claim?
//
// Usage:
//   ttt_xdp_probe --iface IF [--queue Q] --attach native|generic --bind zerocopy|copy
//                 [--match A.B.C.D:PORT] [--listen-ms N]
//
// Every step is forced and then read back from the kernel. Nothing here tries
// a mode and falls back. Each check prints PASS or FAIL. Exit status 0 only if
// every check passed.
//
//   driver          the driver bound to the interface, from sysfs
//   attach          the XDP program attached in the requested mode
//   attach readback the kernel reports that same mode attached
//   bind            the AF_XDP socket bound in the requested mode
//   bind readback   getsockopt(XDP_OPTIONS) agrees
//   numa            the UMEM's node, next to the NIC's node
//   traffic         with --listen-ms: frames arrived on the socket, and the
//                   program's counters and the socket's statistics show no
//                   drops and no feed packets on a queue without a socket

#include <sys/socket.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "ttt/core/clock.hpp"
#include "ttt/xdp/xsk.hpp"

using namespace ttt;

namespace {

int failures = 0;

void check(const char* name, bool ok, const std::string& detail) {
    std::printf("%-16s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++failures;
}

std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string   s;
    std::getline(f, s);
    return f ? s : "n/a";
}

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "ttt_xdp_probe: %s\nsee the comment at the top of apps/xdp_probe.cpp\n",
                 why.c_str());
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    std::string     iface;
    xdp::u32        queue = 0;
    xdp::AttachMode attach = xdp::AttachMode::Native;
    xdp::BindMode   bind = xdp::BindMode::ZeroCopy;
    net::Endpoint   match;
    (void)net::parse_endpoint("0.0.0.0:30001", match);
    long listen_ms = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--iface")
            iface = val();
        else if (a == "--queue")
            queue = static_cast<xdp::u32>(std::strtoul(val().c_str(), nullptr, 10));
        else if (a == "--attach") {
            const std::string v = val();
            if (v != "native" && v != "generic") usage("--attach native|generic");
            attach = v == "native" ? xdp::AttachMode::Native : xdp::AttachMode::Generic;
        } else if (a == "--bind") {
            const std::string v = val();
            if (v != "zerocopy" && v != "copy") usage("--bind zerocopy|copy");
            bind = v == "zerocopy" ? xdp::BindMode::ZeroCopy : xdp::BindMode::Copy;
        } else if (a == "--match") {
            if (!net::parse_endpoint(val(), match)) usage("bad --match");
        } else if (a == "--listen-ms")
            listen_ms = std::strtol(val().c_str(), nullptr, 10);
        else
            usage("unknown option " + a);
    }
    if (iface.empty()) usage("--iface is required");

    const std::string sys = "/sys/class/net/" + iface;
    char              link[512] = {};
    const ssize_t     n = ::readlink((sys + "/device/driver").c_str(), link, sizeof(link) - 1);
    std::string       driver =
        n > 0 ? std::string(link, static_cast<std::size_t>(n)) : "(virtual, no device)";
    driver = driver.substr(driver.find_last_of('/') + 1);
    int rx_queues = 0;
    while (::access((sys + "/queues/rx-" + std::to_string(rx_queues)).c_str(), F_OK) == 0)
        ++rx_queues;
    check("driver", true,
          driver + ", " + std::to_string(rx_queues) + " rx queues, kernel reports " +
              read_line(sys + "/speed") + " Mb/s");

    try {
        xdp::Program prog(iface, attach, match);
        check("attach", true, std::string("forced ") + xdp::to_string(attach));
        const std::string mode = prog.attached_mode();
        check("attach readback", mode == xdp::to_string(attach), "kernel reports " + mode);

        xdp::SocketConfig cfg;
        cfg.queue = queue;
        cfg.bind = bind;
        try {
            xdp::Socket sock(prog, cfg);
            check("bind", true,
                  std::string("forced ") + xdp::to_string(bind) + " on queue " +
                      std::to_string(queue));
            const bool zc = sock.zerocopy();
            check("bind readback", zc == (bind == xdp::BindMode::ZeroCopy),
                  std::string("XDP_OPTIONS says ") + (zc ? "zerocopy" : "copy"));
            check("numa", true,
                  "UMEM on node " + std::to_string(sock.umem_numa_node()) + ", NIC on node " +
                      read_line(sys + "/device/numa_node"));

            if (listen_ms > 0) {
                xdp::XdpPath path(sock, match, 10'000'000, false);
                const auto   end = now_ns() + static_cast<xdp::u64>(listen_ms) * 1'000'000;
                xdp::u64     frames = 0;
                while (now_ns() < end) {
                    frames += path.receive([](std::span<const std::byte>, xdp::u64) {});
                }
                const auto c = prog.counters();
                const auto st = sock.statistics();
                int        napi = 0;
                socklen_t  len = sizeof(napi);
                (void)::getsockopt(sock.fd(), SOL_SOCKET, SO_INCOMING_NAPI_ID, &napi, &len);
                check("traffic",
                      frames > 0 && c.no_socket == 0 && st.rx_dropped == 0 && st.rx_ring_full == 0,
                      "frames " + std::to_string(frames) + ", redirected " +
                          std::to_string(c.redirected) + ", no socket on queue " +
                          std::to_string(c.no_socket) + ", passed " + std::to_string(c.passed) +
                          ", rx_dropped " + std::to_string(st.rx_dropped) + ", rx_ring_full " +
                          std::to_string(st.rx_ring_full) + ", fill_ring_empty " +
                          std::to_string(st.rx_fill_ring_empty_descs) + ", napi id " +
                          std::to_string(napi));
            }
        } catch (const std::exception& e) {
            check("bind", false, e.what());
        }
    } catch (const std::exception& e) {
        check("attach", false, e.what());
    }

    std::printf("%s\n", failures == 0 ? "GATE PASS" : "GATE FAIL");
    return failures == 0 ? 0 : 1;
}

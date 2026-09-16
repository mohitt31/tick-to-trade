// AF_XDP on a veth pair, sender in its own network namespace. Needs root; skips
// otherwise. veth supports native XDP but not zero-copy, which makes it the
// right place to check the gate logic: a forced zero-copy bind has to fail
// outright, a forced native attach has to be reported as native by the kernel,
// and copy mode has to carry every message with the book matching the oracle.
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <system_error>
#include <thread>

#include "itch/book/book_types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/live/feed_server.hpp"
#include "ttt/measure/trailer.hpp"
#include "ttt/rx/bench_receiver.hpp"
#include "ttt/sim/order_gen.hpp"
#include "ttt/xdp/xsk.hpp"

namespace ttt::xdp {
namespace {

using Bytes = std::vector<std::byte>;

const char*         kNs = "ttt_xdp_tx";
const char*         kIfRx = "tttrx0";  // root namespace, where XDP runs
const char*         kIfTx = "ttttx0";  // in the sender's namespace
const net::Endpoint kRxAddr{{10, 99, 0, 1}, 30001};
const net::Endpoint kTxAddr{{10, 99, 0, 2}, 0};

bool sh(const std::string& cmd) { return std::system((cmd + " > /dev/null 2>&1").c_str()) == 0; }

class VethPair : public testing::Test {
protected:
    static void SetUpTestSuite() {
        if (::geteuid() != 0) {
            skip_ = "needs root";
            return;
        }
        sh(std::string("ip netns del ") + kNs);
        sh(std::string("ip link del ") + kIfRx);
        const bool ok = sh(std::string("ip netns add ") + kNs) &&
                        sh(std::string("ip link add ") + kIfRx + " type veth peer name " + kIfTx) &&
                        sh(std::string("ip link set ") + kIfTx + " netns " + kNs) &&
                        sh(std::string("ip addr add 10.99.0.1/24 dev ") + kIfRx) &&
                        sh(std::string("ip link set ") + kIfRx + " up") &&
                        sh(std::string("ip -n ") + kNs + " addr add 10.99.0.2/24 dev " + kIfTx) &&
                        sh(std::string("ip -n ") + kNs + " link set " + kIfTx + " up") &&
                        sh(std::string("ip -n ") + kNs + " link set lo up");
        if (!ok) {
            skip_ = "cannot create a veth pair and namespace here";
        }
    }

    static void TearDownTestSuite() {
        sh(std::string("ip netns del ") + kNs);
        sh(std::string("ip link del ") + kIfRx);
    }

    void SetUp() override {
        if (!skip_.empty()) {
            GTEST_SKIP() << skip_;
        }
    }

    static std::string skip_;
};

std::string VethPair::skip_;

struct Stream {
    Bytes framed;
    u64   messages = 0;
    u64   digest = 0;
};

Stream make_stream() {
    Stream         out;
    Rng            rng(123);
    sim::GenConfig g;
    g.messages = 20'000;
    feed::BookSink oracle("SYM1", feed::BookCapacity{});
    for (const auto& m : sim::generate(rng, g)) {
        const auto b = m.bytes();
        const auto at = out.framed.size();
        out.framed.resize(at + 2 + b.size());
        itch::store_be<itch::u16>(out.framed.data() + at, static_cast<itch::u16>(b.size()));
        std::copy(b.begin(), b.end(), out.framed.begin() + static_cast<long>(at + 2));
        oracle.apply(b);
        ++out.messages;
    }
    out.digest = itch::book::book_digest(oracle.book());
    return out;
}

// Runs a feed server inside the sender namespace. setns() changes the namespace
// of the calling thread only, so the sockets it creates live over there.
live::FeedServerStats send_from_namespace(const Stream& s, std::atomic<bool>& stop) {
    const std::string path = std::string("/var/run/netns/") + kNs;
    const int         nsfd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (nsfd < 0 || ::setns(nsfd, CLONE_NEWNET) != 0) {
        throw std::system_error(errno, std::generic_category(), "setns");
    }
    ::close(nsfd);

    live::FeedServerConfig fc;
    fc.session = mold::Session("XDPTEST");
    fc.iface = kTxAddr.ip;
    fc.feed[0] = kRxAddr;
    fc.feed[1] = net::Endpoint{kRxAddr.ip, 30002};  // not matched: goes to the kernel
    fc.rewind = kTxAddr;
    fc.snapshot = kTxAddr;
    for (auto& sc : fc.stream) {
        sc.packets_per_second = 20'000;
        sc.budget = measure::kMaxPacketWithTrailer;
    }
    fc.trailer = true;
    fc.start_delay_ns = 300'000'000;  // ARP and the receiver settle first
    fc.eos_interval_ns = 10'000'000;
    fc.eos_repeats = 5;

    auto src = [&] {
        return std::make_unique<live::OwnedSource<itch::wire::FrameCursor>>(
            itch::wire::FrameCursor(s.framed));
    };
    live::FeedServer tx(fc, src(), src());
    return tx.run(stop);
}

TEST_F(VethPair, ForcedZeroCopyFailsInsteadOfFallingBack) {
    Program prog(kIfRx, AttachMode::Native, kRxAddr);
    EXPECT_EQ(prog.attached_mode(), "native");

    SocketConfig cfg;
    cfg.bind = BindMode::ZeroCopy;
    try {
        Socket sock(prog, cfg);
        FAIL() << "veth has no zero-copy support, yet a forced zero-copy bind succeeded";
    } catch (const std::system_error& e) {
        // The error names the mode, so a log says exactly what was refused.
        EXPECT_NE(std::string(e.what()).find("zerocopy"), std::string::npos) << e.what();
    }

    cfg.bind = BindMode::Copy;
    Socket sock(prog, cfg);
    EXPECT_FALSE(sock.zerocopy());
}

// The whole path in copy mode, in both attach modes: every message, the book
// matching the oracle, frames accounted for, and the program's own counters
// agreeing with what the socket delivered.
TEST_F(VethPair, CopyModeCarriesTheFeed) {
    const Stream s = make_stream();
    for (AttachMode attach : {AttachMode::Native, AttachMode::Generic}) {
        SCOPED_TRACE(to_string(attach));
        Program prog(kIfRx, attach, kRxAddr);
        EXPECT_EQ(prog.attached_mode(), to_string(attach));

        SocketConfig cfg;
        cfg.bind = BindMode::Copy;
        cfg.frames = 4096;
        Socket  sock(prog, cfg);
        XdpPath path(sock, kRxAddr, 10'000'000, false);
        EXPECT_EQ(sock.free_frames() + sock.with_kernel(), cfg.frames);

        std::atomic<bool>     stop{false};
        live::FeedServerStats tx{};
        std::string           tx_error;
        std::thread           t([&] {
            try {
                tx = send_from_namespace(s, stop);
            } catch (const std::exception& e) {
                tx_error = e.what();
            }
        });

        rx::BenchConfig bc;
        bc.line.session = mold::Session("XDPTEST");
        bc.idle_timeout_ns = 3'000'000'000;
        const rx::BenchResult r = rx::run_bench(path, bc, stop);
        stop = true;
        t.join();
        ASSERT_TRUE(tx_error.empty()) << tx_error;

        EXPECT_EQ(r.rx.stopped_because, "end of session, book complete");
        EXPECT_EQ(r.rx.applied, s.messages);
        EXPECT_EQ(r.rx.digest, s.digest);
        EXPECT_EQ(r.missing, 0u);
        EXPECT_EQ(r.path.errors, 0u);
        EXPECT_EQ(r.rx.latency->without_trailer, 0u);

        const ProgramCounters c = prog.counters();
        EXPECT_GE(c.redirected, r.path.datagrams);  // the receiver may stop before the last EOS
        EXPECT_EQ(c.no_socket, 0u);
        EXPECT_GT(c.passed, 0u);  // feed B and ARP went to the kernel

        const xdp_statistics st = sock.statistics();
        EXPECT_EQ(st.rx_dropped, 0u);
        EXPECT_EQ(st.rx_ring_full, 0u);
        EXPECT_EQ(sock.free_frames() + sock.with_kernel(), cfg.frames);
        // A kernel built without NUMA (Docker's is) cannot say; one with it must.
        if (::access("/sys/devices/system/node", F_OK) == 0) {
            EXPECT_GE(sock.umem_numa_node(), 0);
        } else {
            EXPECT_EQ(sock.umem_numa_node(), -1);
        }
    }
}

// The trap the whole gate exists for: the feed arrives on a queue with no
// socket. Nothing errors anywhere; only the program's counter says so.
TEST_F(VethPair, PacketsForAQueueWithoutASocketAreCounted) {
    const Stream s = make_stream();
    Program      prog(kIfRx, AttachMode::Native, kRxAddr);  // no socket at all

    std::atomic<bool> stop{false};
    std::thread       t([&] {
        try {
            (void)send_from_namespace(s, stop);
        } catch (...) {
        }
    });
    t.join();
    const ProgramCounters c = prog.counters();
    EXPECT_EQ(c.redirected, 0u);
    EXPECT_GT(c.no_socket, 0u);
}

}  // namespace
}  // namespace ttt::xdp

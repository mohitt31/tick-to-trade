// Every Linux receive path against the same sender over loopback: all messages
// arrive, the book matches the oracle, and each path's own accounting is
// consistent with what it is supposed to be doing. Functional only; these run
// in a container and are never measurements.
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/live/feed_server.hpp"
#include "ttt/rx/bench_receiver.hpp"
#include "ttt/rx/socket_paths.hpp"
#include "ttt/rx/timestamp_path.hpp"
#include "ttt/rx/uring_path.hpp"
#include "ttt/sim/order_gen.hpp"

namespace ttt::rx {
namespace {

using Bytes = std::vector<std::byte>;

struct Stream {
    Bytes framed;
    u64   messages = 0;
    u64   digest = 0;
};

const Stream& stream() {
    static const Stream s = [] {
        Stream         out;
        Rng            rng(77);
        sim::GenConfig g;
        g.messages = 30'000;
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
    }();
    return s;
}

std::unique_ptr<live::AnySource> source() {
    return std::make_unique<live::OwnedSource<itch::wire::FrameCursor>>(
        itch::wire::FrameCursor(stream().framed));
}

struct PathRun {
    BenchResult           result;
    live::FeedServerStats tx;
};

// Builds the receive socket, lets make_path wrap it, and runs a sender at it.
template <class MakePath>
PathRun run_with(MakePath make_path, u64 pps = 20'000, std::size_t budget = 600,
                 u64 warmup_ns = 0) {
    net::UdpOptions o;
    o.bind = net::Endpoint{net::kLoopback, 0};
    o.rcvbuf = 8 << 20;
    net::Fd feed = net::udp_socket(o);
    net::Fd sink = net::udp_socket(o);  // feed B goes nowhere interesting

    live::FeedServerConfig fc;
    fc.session = mold::Session("RXPATHS");
    fc.feed[0] = net::local_endpoint(feed.get());
    fc.feed[1] = net::local_endpoint(sink.get());
    fc.rewind = net::Endpoint{net::kLoopback, 0};
    fc.snapshot = net::Endpoint{net::kLoopback, 0};
    for (auto& s : fc.stream) {
        s.packets_per_second = pps;
        s.budget = budget;
    }
    fc.trailer = true;
    fc.start_delay_ns = 50'000'000;
    fc.eos_interval_ns = 10'000'000;
    fc.eos_repeats = 5;

    auto path = make_path(feed.get());

    live::FeedServer      tx(fc, source(), source());
    std::atomic<bool>     stop{false};
    live::FeedServerStats tx_stats;
    std::thread           t([&] { tx_stats = tx.run(stop); });

    BenchConfig bc;
    bc.line.session = fc.session;
    bc.idle_timeout_ns = 2'000'000'000;
    bc.warmup_ns = warmup_ns;
    BenchResult r = run_bench(*path, bc, stop);
    t.join();
    return {std::move(r), tx_stats};
}

void expect_complete(const PathRun& run) {
    const auto& r = run.result;
    ASSERT_EQ(r.rx.stopped_because, "end of session, book complete");
    EXPECT_EQ(r.rx.applied, stream().messages);
    EXPECT_EQ(r.rx.digest, stream().digest);
    EXPECT_EQ(r.missing, 0u);
    EXPECT_EQ(r.unapplied, 0u);
    ASSERT_TRUE(r.rx.latency.has_value());
    EXPECT_EQ(r.rx.latency->without_trailer, 0u);
    EXPECT_EQ(r.rx.latency->intended_to_recv.count(), r.path.datagrams);  // no warmup here
    EXPECT_EQ(r.path.truncated, 0u);
    EXPECT_EQ(r.path.errors, 0u);
    EXPECT_GT(r.path.wakeups, 0u);
    EXPECT_EQ(r.batch.count(), r.path.wakeups);
}

TEST(RxPaths, Recvfrom) {
    const PathRun run =
        run_with([](int fd) { return std::make_unique<RecvfromPath>(fd, 10'000'000); });
    expect_complete(run);
    // One datagram per call, always.
    EXPECT_EQ(run.result.batch.max(), 1);
    EXPECT_EQ(run.result.path.wakeups, run.result.path.datagrams);
}

// A warmup must only stop latency being recorded. The first version also
// stopped the trailer being stripped, so every warmup datagram reached the
// decoder with 32 extra bytes and was rejected.
TEST(RxPaths, WarmupSkipsRecordingNotDecoding) {
    const PathRun run =
        run_with([](int fd) { return std::make_unique<RecvfromPath>(fd, 10'000'000); }, 5'000, 600,
                 100'000'000);  // about 300 ms of traffic, the first 100 ms unrecorded
    const auto& r = run.result;
    ASSERT_EQ(r.rx.stopped_because, "end of session, book complete");
    EXPECT_EQ(r.rx.applied, stream().messages);
    EXPECT_EQ(r.rx.line.decode_errors, 0u);
    EXPECT_EQ(r.missing, 0u);
    EXPECT_GT(r.rx.latency->intended_to_recv.count(), 0u);
    EXPECT_LT(r.rx.latency->intended_to_recv.count(), r.path.datagrams);
}

TEST(RxPaths, RecvmmsgBatchesButNeverExceedsTheBatch) {
    for (unsigned batch : {1u, 8u, 64u}) {
        SCOPED_TRACE(testing::Message() << "batch " << batch);
        // A fast sender so datagrams queue up and batches fill.
        const PathRun run = run_with(
            [batch](int fd) { return std::make_unique<RecvmmsgPath>(fd, 10'000'000, batch); },
            200'000, 200);
        expect_complete(run);
        EXPECT_LE(run.result.batch.max(), static_cast<itch::i64>(batch));
        EXPECT_EQ(run.result.path.syscalls, run.result.path.wakeups + run.result.path.empty);
    }
}

TEST(RxPaths, EpollLevelReadsOncePerWakeup) {
    const PathRun run = run_with(
        [](int fd) { return std::make_unique<EpollPath>(fd, 10'000'000, Trigger::Level); });
    expect_complete(run);
    EXPECT_EQ(run.result.batch.max(), 1);
    // epoll_pwait2 plus one recv for every wakeup.
    EXPECT_GE(run.result.path.syscalls, 2 * run.result.path.wakeups);
}

TEST(RxPaths, EpollEdgeDrainsToEagain) {
    const PathRun run =
        run_with([](int fd) { return std::make_unique<EpollPath>(fd, 10'000'000, Trigger::Edge); },
                 200'000, 200);
    expect_complete(run);
    // Every wakeup ends with a recv that found nothing: calls are at least the
    // wait, each datagram, and the final EAGAIN.
    EXPECT_GE(run.result.path.syscalls, 2 * run.result.path.wakeups + run.result.path.datagrams);
}

// Software receive timestamps come with every datagram, and the time from the
// kernel's stamp to recvmsg returning is positive and sane. Hardware stamps are
// asked for too; loopback cannot give them, and the path must say so rather
// than record nothing quietly.
TEST(RxPaths, KernelReceiveTimestamps) {
    // The whole pipeline over the timestamping path first.
    const PathRun run =
        run_with([](int fd) { return std::make_unique<TimestampPath>(fd, 10'000'000, true); });
    expect_complete(run);

    // run_with does not keep the path, so its timestamp histograms are checked
    // on a path this test owns.
    net::UdpOptions o;
    o.bind = net::Endpoint{net::kLoopback, 0};
    net::Fd                         rx = net::udp_socket(o);
    net::Fd                         tx = net::udp_socket(o);
    TimestampPath                   p(rx.get(), 100'000'000, true);
    const std::array<std::byte, 64> payload{};
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(net::send_to(tx.get(), net::local_endpoint(rx.get()), payload), net::Io::Ok);
    }
    std::size_t got = 0;
    while (got < 100 && p.receive([](std::span<const std::byte>, u64) {}) == 1) {
        ++got;
    }
    EXPECT_EQ(got, 100u);
    EXPECT_EQ(p.without_software(), 0u);
    EXPECT_EQ(p.stack_to_user().count(), 100u);
    EXPECT_EQ(p.stack_to_user().clamped_low(), 0u);  // the stamp is never after the return
    EXPECT_LT(p.stack_to_user().max(), 1'000'000'000);
    EXPECT_EQ(p.without_hardware(), 100u);  // loopback has no NIC to stamp with
    EXPECT_EQ(p.nic_to_user().count(), 0u);
}

TEST(RxPaths, IoUringAllModes) {
    for (UringMode mode : {UringMode::Plain, UringMode::Sqpoll, UringMode::Defer}) {
        SCOPED_TRACE(testing::Message() << "mode " << static_cast<int>(mode));
        const PathRun run =
            run_with([mode](int fd) { return std::make_unique<UringPath>(fd, 10'000'000, mode); },
                     100'000, 300);
        expect_complete(run);
    }
}

}  // namespace
}  // namespace ttt::rx

namespace ttt::rx {
namespace {

// With a threshold of 1 ns every recorded datagram is an outlier, so the log
// has to hold exactly the recorded ones, in receive order, each with its first
// sequence number, and never more than its capacity.
TEST(RxPaths, OutlierLog) {
    net::UdpOptions o;
    o.bind = net::Endpoint{net::kLoopback, 0};
    o.rcvbuf = 8 << 20;
    net::Fd feed = net::udp_socket(o);
    net::Fd sink = net::udp_socket(o);

    live::FeedServerConfig fc;
    fc.session = mold::Session("OUTLIERS");
    fc.feed[0] = net::local_endpoint(feed.get());
    fc.feed[1] = net::local_endpoint(sink.get());
    fc.rewind = net::Endpoint{net::kLoopback, 0};
    fc.snapshot = net::Endpoint{net::kLoopback, 0};
    for (auto& s : fc.stream) {
        s.packets_per_second = 20'000;
        s.budget = 600;
    }
    fc.trailer = true;
    fc.start_delay_ns = 50'000'000;
    fc.eos_interval_ns = 10'000'000;
    fc.eos_repeats = 5;

    RecvfromPath      path(feed.get(), 10'000'000);
    live::FeedServer  tx(fc, source(), source());
    std::atomic<bool> stop{false};
    std::thread       t([&] { (void)tx.run(stop); });
    BenchConfig       bc;
    bc.line.session = fc.session;
    bc.outlier_threshold_ns = 1;
    bc.outlier_capacity = 500;
    const BenchResult r = run_bench(path, bc, stop);
    t.join();

    ASSERT_EQ(r.rx.stopped_because, "end of session, book complete");
    const u64 recorded = r.rx.latency->intended_to_recv.count();
    ASSERT_GT(recorded, 500u);
    EXPECT_EQ(r.outliers.size(), 500u);
    EXPECT_EQ(r.outliers.size() + r.outliers_dropped, recorded);
    EXPECT_EQ(r.outliers.front().sequence, 1u);
    for (std::size_t i = 1; i < r.outliers.size(); ++i) {
        EXPECT_GE(r.outliers[i].recv_ns, r.outliers[i - 1].recv_ns);
        EXPECT_GT(r.outliers[i].sequence, r.outliers[i - 1].sequence);
        EXPECT_GE(r.outliers[i].handled, r.outliers[i].intended_to_recv);
    }
}

}  // namespace
}  // namespace ttt::rx

// The whole pipeline over real sockets on this machine: feed server and
// receiver in two threads, talking UDP and TCP over loopback. The receiver's
// final book has to match a book fed the clean stream, with and without faults
// injected at the sender.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <system_error>
#include <thread>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/live/feed_server.hpp"
#include "ttt/live/receiver.hpp"
#include "ttt/sim/order_gen.hpp"

namespace ttt::live {
namespace {

using Bytes = std::vector<std::byte>;
using itch::u64;

struct Stream {
    std::vector<itch_out::Message> msgs;
    Bytes                          framed;
    u64                            digest = 0;
};

Stream make_stream(u64 seed, itch::u32 n) {
    Stream         s;
    Rng            rng(seed);
    sim::GenConfig g;
    g.messages = n;
    s.msgs = sim::generate(rng, g);
    feed::BookSink oracle("SYM1", feed::BookCapacity{});
    for (const auto& m : s.msgs) {
        const auto b = m.bytes();
        const auto at = s.framed.size();
        s.framed.resize(at + 2 + b.size());
        itch::store_be<itch::u16>(s.framed.data() + at, static_cast<itch::u16>(b.size()));
        std::copy(b.begin(), b.end(), s.framed.begin() + static_cast<long>(at + 2));
        oracle.apply(b);
    }
    s.digest = itch::book::book_digest(oracle.book());
    return s;
}

std::unique_ptr<AnySource> source(const Bytes& framed) {
    return std::make_unique<OwnedSource<itch::wire::FrameCursor>>(itch::wire::FrameCursor(framed));
}

struct Outcome {
    ReceiverResult  rx;
    FeedServerStats tx;
};

Outcome run_pair(const Stream& s, ReceiverConfig rc, FeedServerConfig fc) {
    Receiver rx(rc);
    for (int f = 0; f < 2; ++f) {
        if (!fc.feed[f].multicast()) {
            fc.feed[f] = rx.feed_endpoint(f);
        }
    }
    FeedServer tx(fc, source(s.framed), source(s.framed));
    rx.set_servers(tx.rewind_endpoint(), tx.snapshot_endpoint());

    std::atomic<bool> stop{false};
    FeedServerStats   tx_stats;
    std::thread       t([&] { tx_stats = tx.run(stop); });
    ReceiverResult    r = rx.run(stop);
    // The receiver stops as soon as its book is complete; let the sender finish
    // its end-of-session repeats rather than cutting it off.
    t.join();
    return {r, tx_stats};
}

FeedServerConfig server_config(u64 pps) {
    FeedServerConfig fc;
    fc.session = mold::Session("LOOPBACK");
    fc.rewind = net::Endpoint{net::kLoopback, 0};
    fc.snapshot = net::Endpoint{net::kLoopback, 0};
    for (int f = 0; f < 2; ++f) {
        fc.stream[f].pacing = replay::Pacing::FixedRate;
        fc.stream[f].packets_per_second = pps;
    }
    fc.stream[0].budget = 1472;
    fc.stream[1].budget = 300;  // feed B splits the same messages differently
    fc.start_delay_ns = 50'000'000;
    fc.eos_interval_ns = 20'000'000;
    fc.eos_repeats = 5;
    return fc;
}

ReceiverConfig receiver_config() {
    ReceiverConfig rc;
    rc.line.session = mold::Session("LOOPBACK");
    rc.line.arb_wait_ns = 1'000'000;
    rc.line.retransmit_timeout_ns = 20'000'000;
    rc.line.max_retransmit_attempts = 3;
    rc.line.max_retransmit_count = 1000;
    rc.line.snapshot_timeout_ns = 200'000'000;
    for (int f = 0; f < 2; ++f) {
        rc.feed[f] = net::Endpoint{net::kLoopback, 0};
    }
    rc.idle_timeout_ns = 3'000'000'000;
    return rc;
}

TEST(Loopback, CleanRunMatchesTheOracle) {
    const Stream s = make_stream(1, 20'000);
    const auto   out = run_pair(s, receiver_config(), server_config(5'000));
    ASSERT_EQ(out.rx.stopped_because, "end of session, book complete");
    EXPECT_EQ(out.rx.applied, s.msgs.size());
    EXPECT_EQ(out.rx.digest, s.digest);
    EXPECT_FALSE(out.rx.stale);
    EXPECT_EQ(out.rx.line.snapshots_applied, 0u);
    EXPECT_EQ(out.tx.messages, s.msgs.size());
}

// Loss, duplication and reordering on both feeds, lost retransmission replies
// and snapshots cut mid-transfer. Which recovery path a gap takes must not
// depend on how fast the machine is (a sanitizer build is several times
// slower), so the outages are given as sequence ranges lost on both feeds: a
// short one that only a retransmission can fill, and one longer than a
// retransmission may ask for, which only a snapshot can.
TEST(Loopback, FaultyRunRecoversToTheOracle) {
    const Stream     s = make_stream(2, 40'000);
    FeedServerConfig fc = server_config(4'000);
    fc.faults_on = true;
    fc.faults.seed = 7;
    for (int f = 0; f < 2; ++f) {
        fc.faults.feed[f].drop_ppm = 50'000;
        fc.faults.feed[f].dup_ppm = 30'000;
        fc.faults.feed[f].jitter_ns = 2'000'000;
    }
    fc.faults.joint_outages.emplace_back(5'000, 5'300);    // only a retransmission fills this
    fc.faults.joint_outages.emplace_back(15'000, 25'000);  // more than one may ask for
    fc.faults.response_drop_ppm = 200'000;
    fc.faults.snapshot_cut_ppm = 300'000;

    ReceiverConfig rc = receiver_config();
    rc.line.max_retransmit_count = 5000;

    const auto out = run_pair(s, rc, fc);
    std::printf("dropped %llu/%llu  retransmit requests %llu  snapshots %llu  cut %llu\n",
                static_cast<unsigned long long>(out.tx.dropped[0]),
                static_cast<unsigned long long>(out.tx.dropped[1]),
                static_cast<unsigned long long>(out.rx.line.retransmit_requests),
                static_cast<unsigned long long>(out.rx.line.snapshots_applied),
                static_cast<unsigned long long>(out.tx.snapshots_cut));
    ASSERT_EQ(out.rx.stopped_because, "end of session, book complete");
    EXPECT_EQ(out.rx.applied, s.msgs.size());
    EXPECT_EQ(out.rx.digest, s.digest);
    EXPECT_GT(out.rx.line.duplicates, 0u);
    EXPECT_GT(out.rx.line.retransmit_requests, 0u);
    EXPECT_GT(out.rx.line.snapshots_applied, 0u);
}

TEST(Loopback, MulticastGroups) {
    const Stream     s = make_stream(3, 10'000);
    ReceiverConfig   rc = receiver_config();
    FeedServerConfig fc = server_config(5'000);
    const auto       port = static_cast<itch::u16>(20000 + (::getpid() % 20000));
    rc.feed[0] = fc.feed[0] = net::Endpoint{{239, 255, 71, 1}, port};
    rc.feed[1] = fc.feed[1] = net::Endpoint{{239, 255, 71, 2}, port};
    try {
        Receiver probe(rc);  // joins; throws where loopback cannot do multicast
    } catch (const std::system_error& e) {
        GTEST_SKIP() << "no multicast on loopback here: " << e.what();
    }
    const auto out = run_pair(s, rc, fc);
    ASSERT_EQ(out.rx.stopped_because, "end of session, book complete");
    EXPECT_EQ(out.rx.applied, s.msgs.size());
    EXPECT_EQ(out.rx.digest, s.digest);
}

}  // namespace
}  // namespace ttt::live

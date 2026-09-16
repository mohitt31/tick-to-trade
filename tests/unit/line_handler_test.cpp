#include "ttt/feed/line_handler.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/itch/encode.hpp"
#include "ttt/mold/packer.hpp"

namespace ttt::feed {
namespace {

using Bytes = std::vector<std::byte>;
const mold::Session kS{"LINE"};

// Records everything the handler asks for.
struct Recorder {
    std::vector<u64>                 applied;
    std::vector<std::pair<u64, u16>> retransmits;
    int                              snapshot_requests = 0;
    std::vector<u64>                 snapshots;  // end_snapshot sequence numbers
    std::vector<std::string>         snapshot_msgs;

    void apply(u64 seq, std::span<const std::byte>) { applied.push_back(seq); }
    void begin_snapshot(u64) { snapshot_msgs.clear(); }
    void apply_snapshot(std::span<const std::byte> m) {
        snapshot_msgs.push_back(std::string(1, static_cast<char>(m[0])));
    }
    void end_snapshot(u64 seq) { snapshots.push_back(seq); }
    void request_retransmit(u64 seq, u16 count) { retransmits.push_back({seq, count}); }
    void request_snapshot() { ++snapshot_requests; }
};

// A packet carrying messages [first, first + n), each a delete for order `seq`
// so every message is distinct.
Bytes packet(u64 first, u16 n, const mold::Session& s = kS) {
    mold::Packer p(s, first, mold::kMaxPayloadStandardMtu);
    for (u16 i = 0; i < n; ++i) {
        p.add(itch_out::order_delete({1, 0, 0}, first + i).bytes());
    }
    const auto b = p.finish();
    return Bytes(b.begin(), b.end());
}

Bytes heartbeat(u64 next) {
    mold::Packer p(kS, next, mold::kMaxPayloadStandardMtu);
    const auto   b = p.heartbeat();
    return Bytes(b.begin(), b.end());
}

std::vector<u64> iota(u64 from, u64 to) {
    std::vector<u64> v;
    for (u64 i = from; i < to; ++i) {
        v.push_back(i);
    }
    return v;
}

LineConfig cfg() {
    LineConfig c;
    c.session = kS;
    c.arb_wait_ns = 100;
    c.retransmit_timeout_ns = 1000;
    c.max_retransmit_attempts = 2;
    c.max_retransmit_count = 50;
    c.snapshot_timeout_ns = 10'000;
    c.buffer_capacity = 64;
    return c;
}

TEST(LineHandler, InOrderIsAppliedAndLive) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(6, 5), 1);
    EXPECT_EQ(r.applied, iota(1, 11));
    EXPECT_EQ(h.state(), State::Live);
    EXPECT_EQ(h.next_deadline(), kNever);
    EXPECT_EQ(h.stats().first_wins[0], 10u);
}

TEST(LineHandler, FeedBFillsAGapWithoutARetransmission) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(11, 5), 1);  // 6..10 lost on A
    EXPECT_EQ(h.state(), State::GapWait);
    EXPECT_TRUE(h.stale());
    EXPECT_EQ(h.next_deadline(), 101u);

    h.on_packet(Source::B, packet(1, 10), 50);  // B is behind, and fills it
    EXPECT_EQ(r.applied, iota(1, 16));
    EXPECT_EQ(h.state(), State::Live);
    EXPECT_TRUE(r.retransmits.empty());
    EXPECT_EQ(h.stats().duplicates, 5u);
    EXPECT_EQ(h.stats().first_wins[1], 5u);
    EXPECT_EQ(h.stats().applied_from_buffer, 5u);
}

// Feed B split the same messages differently; a packet straddling the next
// expected message contributes exactly the part not yet applied.
TEST(LineHandler, StraddlingPacketAppliesOnlyTheNewPart) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 7), 0);
    h.on_packet(Source::B, packet(4, 8), 1);  // 4..11: 4..7 old, 8..11 new
    EXPECT_EQ(r.applied, iota(1, 12));
    EXPECT_EQ(h.stats().duplicates, 4u);
}

TEST(LineHandler, RetransmitAsksForTheFirstHoleOnly) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(9, 2), 1);   // hole 6..8
    h.on_packet(Source::A, packet(15, 2), 2);  // hole 11..14
    h.on_timer(100);                           // not yet
    EXPECT_TRUE(r.retransmits.empty());
    h.on_timer(101);
    ASSERT_EQ(r.retransmits.size(), 1u);
    EXPECT_EQ(r.retransmits[0], (std::pair<u64, u16>{6, 3}));
    EXPECT_EQ(h.state(), State::Retransmitting);

    h.on_packet(Source::Rewind, packet(6, 3), 200);
    EXPECT_EQ(r.applied, iota(1, 11));
    EXPECT_EQ(h.state(), State::Retransmitting);  // 11..14 still missing

    h.on_timer(1101);  // the hole moved: ask for the next one with a fresh budget
    ASSERT_EQ(r.retransmits.size(), 2u);
    EXPECT_EQ(r.retransmits[1], (std::pair<u64, u16>{11, 4}));
    h.on_packet(Source::Rewind, packet(11, 4), 1200);
    EXPECT_EQ(r.applied, iota(1, 17));
    EXPECT_EQ(h.state(), State::Live);
}

TEST(LineHandler, UnansweredRetransmitsEscalateToASnapshot) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(8, 1), 0);
    h.on_timer(100);
    h.on_timer(1100);
    EXPECT_EQ(r.retransmits.size(), 2u);
    EXPECT_EQ(r.snapshot_requests, 0);
    h.on_timer(2100);  // two attempts, no progress
    EXPECT_EQ(r.snapshot_requests, 1);
    EXPECT_EQ(h.state(), State::Snapshotting);
    h.on_timer(12'100);  // snapshot timed out: ask again
    EXPECT_EQ(r.snapshot_requests, 2);
}

TEST(LineHandler, LargeGapGoesStraightToSnapshot) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(100, 1), 0);  // 94 missing > 50
    h.on_timer(100);
    EXPECT_TRUE(r.retransmits.empty());
    EXPECT_EQ(r.snapshot_requests, 1);
}

TEST(LineHandler, HeartbeatRevealsTailLoss) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    EXPECT_EQ(h.state(), State::Live);
    h.on_packet(Source::A, heartbeat(9), 10);  // 6..8 were sent and lost
    EXPECT_EQ(h.state(), State::GapWait);
    EXPECT_EQ(h.frontier(), 9u);
}

Bytes snapshot(u64 last, int orders) {
    Bytes          out;
    SnapshotWriter w(out, kS, last);
    w.add(itch_out::stock_directory({1, 0, 0}, "SYM1").bytes());
    for (int i = 0; i < orders; ++i) {
        w.add(itch_out::add_order({1, 0, 0}, static_cast<u64>(i + 1), 'B', 100, "SYM1", 10000)
                  .bytes());
    }
    return out;
}

TEST(LineHandler, SnapshotJumpsAheadAndDrainsWhatFollows) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 5), 0);
    h.on_packet(Source::A, packet(200, 1), 0);
    h.on_packet(Source::A, packet(201, 3), 0);
    h.on_timer(100);
    ASSERT_EQ(h.state(), State::Snapshotting);

    // Arrives while snapshotting: held, not applied.
    h.on_packet(Source::B, packet(204, 2), 500);
    EXPECT_EQ(r.applied, iota(1, 6));

    h.on_snapshot(snapshot(199, 3), 600);
    ASSERT_EQ(r.snapshots, (std::vector<u64>{199}));
    EXPECT_EQ(r.snapshot_msgs, (std::vector<std::string>{"R", "A", "A", "A"}));
    EXPECT_EQ(h.next_expected(), 206u);
    std::vector<u64> want = iota(1, 6);
    for (u64 q = 200; q < 206; ++q) {
        want.push_back(q);
    }
    EXPECT_EQ(r.applied, want);
    EXPECT_EQ(h.state(), State::Live);
}

TEST(LineHandler, SnapshotsThatAreUnwantedOldOrBrokenAreIgnored) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 20), 0);

    h.on_snapshot(snapshot(30, 1), 1);  // not snapshotting
    EXPECT_EQ(h.stats().snapshots_ignored, 1u);

    h.on_packet(Source::A, packet(200, 1), 2);
    h.on_timer(102);
    ASSERT_EQ(h.state(), State::Snapshotting);
    h.on_snapshot(snapshot(10, 1), 3);  // older than what we have
    EXPECT_EQ(h.stats().snapshots_ignored, 2u);

    Bytes cut = snapshot(150, 5);
    cut.pop_back();  // connection cut mid-transfer
    h.on_snapshot(cut, 4);
    EXPECT_EQ(h.stats().snapshot_errors, 1u);
    EXPECT_TRUE(r.snapshots.empty());
    EXPECT_EQ(h.next_expected(), 21u);
}

TEST(LineHandler, WrongSessionAndGarbageAreCountedNotApplied) {
    Recorder              r;
    LineHandler<Recorder> h(cfg(), r);
    h.on_packet(Source::A, packet(1, 3, mold::Session("OTHER")), 0);
    Bytes junk = packet(1, 3);
    junk.pop_back();
    h.on_packet(Source::A, junk, 0);
    EXPECT_TRUE(r.applied.empty());
    EXPECT_EQ(h.stats().wrong_session, 1u);
    EXPECT_EQ(h.stats().decode_errors, 1u);
    EXPECT_EQ(h.state(), State::Live);
}

// A buffer smaller than the gap: the newest messages win their slots, nothing
// wrong is ever applied, and what was evicted is recovered by retransmission.
TEST(LineHandler, TinyBufferNeverAppliesTheWrongMessage) {
    LineConfig c = cfg();
    c.buffer_capacity = 4;
    Recorder              r;
    LineHandler<Recorder> h(c, r);
    h.on_packet(Source::A, packet(1, 1), 0);
    for (u64 q = 3; q <= 12; ++q) {
        h.on_packet(Source::A, packet(q, 1), 1);  // 2 is missing
    }
    EXPECT_GT(h.stats().evicted, 0u);
    h.on_packet(Source::Rewind, packet(2, 1), 2);
    // 9..12 survived in the four slots, 3..8 were evicted, so only 2 applies.
    EXPECT_EQ(r.applied, iota(1, 3));
    h.on_timer(102);
    ASSERT_FALSE(r.retransmits.empty());
    EXPECT_EQ(r.retransmits.back(), (std::pair<u64, u16>{3, 6}));
    h.on_packet(Source::Rewind, packet(3, 6), 3);
    EXPECT_EQ(r.applied, iota(1, 13));
}

}  // namespace
}  // namespace ttt::feed

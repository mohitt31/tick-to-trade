#include "ttt/replay/servers.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "itch/book/book_types.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/seq_slots.hpp"
#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/sim/order_gen.hpp"

namespace ttt {
namespace {

using Bytes = std::vector<std::byte>;
using itch::u16;
using itch::u32;
using itch::u64;
const mold::Session          kS{"SRV"};
constexpr feed::BookCapacity kCap{16'384, 8'192, 4'096};

TEST(SeqSlots, CollisionsKeepTheNewestAndNeverLie) {
    feed::SeqSlots s(4);
    const Bytes    a{std::byte{1}}, b{std::byte{2}};
    EXPECT_FALSE(s.put(1, a).replaced);
    EXPECT_EQ(s.find(1).size(), 1u);
    const auto p = s.put(5, b);  // same slot as 1
    EXPECT_TRUE(p.replaced);
    EXPECT_EQ(p.old_seq, 1u);
    EXPECT_TRUE(s.find(1).empty());  // never returns 5's bytes for 1
    EXPECT_EQ(s.find(5)[0], std::byte{2});
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s.erase(1));
    EXPECT_TRUE(s.erase(5));
    EXPECT_EQ(s.size(), 0u);
}

TEST(Mold, RequestRoundTrip) {
    const auto   r = mold::encode_request(kS, 42, 7);
    mold::Header h;
    ASSERT_TRUE(mold::decode_request(r, h));
    EXPECT_EQ(h.sequence, 42u);
    EXPECT_EQ(h.count, 7u);
    Bytes longer(r.begin(), r.end());
    longer.push_back(std::byte{0});
    EXPECT_FALSE(mold::decode_request(longer, h));
    auto zero = r;
    itch::store_be<u16>(zero.data() + 18, 0);
    EXPECT_FALSE(mold::decode_request(zero, h));
}

std::vector<itch_out::Message> msgs_for(u64 seed, u32 n) {
    Rng            rng(seed);
    sim::GenConfig g;
    g.messages = n;
    return sim::generate(rng, g);
}

TEST(RewindServer, ServesWhatItHoldsInBudgetSizedPackets) {
    const auto           msgs = msgs_for(1, 500);
    replay::RewindServer srv(kS, 256, 200);
    for (u64 i = 0; i < msgs.size(); ++i) {
        srv.publish(i + 1, msgs[i].bytes());
    }

    std::vector<Bytes> out;
    auto emit = [&](std::span<const std::byte> p) { out.push_back(Bytes(p.begin(), p.end())); };

    // Recent: answered, split by budget, contiguous, byte-identical.
    ASSERT_TRUE(srv.serve(mold::encode_request(kS, 400, 50), emit));
    ASSERT_GT(out.size(), 1u);
    u64 q = 400;
    for (const Bytes& p : out) {
        EXPECT_LE(p.size(), 200u);
        mold::PacketView v;
        ASSERT_EQ(mold::decode(p, v), mold::DecodeError::None);
        EXPECT_EQ(v.header().sequence, q);
        for (auto m : v) {
            const auto want = msgs[q - 1].bytes();
            EXPECT_TRUE(std::equal(m.begin(), m.end(), want.begin(), want.end()));
            ++q;
        }
    }
    EXPECT_EQ(q, 450u);

    // Too old, partly old, in the future, wrong session: nothing at all.
    out.clear();
    EXPECT_FALSE(srv.serve(mold::encode_request(kS, 10, 5), emit));
    EXPECT_FALSE(srv.serve(mold::encode_request(kS, 240, 20), emit));
    EXPECT_FALSE(srv.serve(mold::encode_request(kS, msgs.size(), 5), emit));
    EXPECT_FALSE(srv.serve(mold::encode_request(mold::Session("X"), 400, 5), emit));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(srv.unavailable(), 3u);
    EXPECT_EQ(srv.rejected(), 1u);
}

// The core property of a snapshot: at any point in any stream, a book rebuilt
// from the snapshot and then fed the rest of the stream is identical, priority
// included, to a book fed the whole stream.
TEST(SnapshotServer, RebuildAtAnyPointMatches) {
    for (u64 seed = 1; seed <= 40; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        const auto msgs = msgs_for(seed, 1500);
        Rng        rng(seed * 31);
        const u64  cut = rng.range(0, msgs.size());

        replay::SnapshotServer srv(kS, "SYM1", kCap);
        feed::BookSink         full("SYM1", kCap);
        for (u64 i = 0; i < cut; ++i) {
            srv.publish(i + 1, msgs[i].bytes());
            full.apply(msgs[i].bytes());
        }
        Bytes snap;
        srv.build(snap);

        feed::SnapshotView v;
        ASSERT_TRUE(feed::decode_snapshot(snap, v));
        EXPECT_EQ(v.last_sequence(), cut);

        feed::BookSink rebuilt("SYM1", kCap);
        v.for_each([&](std::span<const std::byte> m) { ASSERT_TRUE(rebuilt.apply(m)); });
        ASSERT_EQ(itch::book::book_digest(rebuilt.book()), itch::book::book_digest(full.book()));

        for (u64 i = cut; i < msgs.size(); ++i) {
            rebuilt.apply(msgs[i].bytes());
            full.apply(msgs[i].bytes());
        }
        ASSERT_EQ(itch::book::book_digest(rebuilt.book()), itch::book::book_digest(full.book()));
        rebuilt.book().validate();
    }
}

TEST(SnapshotWire, EveryTruncationIsRejected) {
    const auto             msgs = msgs_for(3, 300);
    replay::SnapshotServer srv(kS, "SYM1", kCap);
    for (u64 i = 0; i < msgs.size(); ++i) {
        srv.publish(i + 1, msgs[i].bytes());
    }
    Bytes snap;
    srv.build(snap);
    feed::SnapshotView v;
    ASSERT_TRUE(feed::decode_snapshot(snap, v));
    ASSERT_GT(v.count(), 1u);
    for (std::size_t n = 0; n < snap.size(); ++n) {
        ASSERT_FALSE(feed::decode_snapshot(std::span(snap).first(n), v)) << n;
    }
}

// Generated streams are valid order flow: a book at full checking applies
// every message and passes its structural validation.
TEST(OrderGen, StreamsAreValidForTheBook) {
    for (u64 seed = 1; seed <= 50; ++seed) {
        const auto msgs = msgs_for(seed, 3000);
        for (u32 s = 0; s < 3; ++s) {
            feed::BookSink b(sim::symbol_name(s), kCap);
            for (const auto& m : msgs) {
                ASSERT_TRUE(b.apply(m.bytes()));
            }
            b.book().validate();
            EXPECT_TRUE(b.builder().resolved());
            EXPECT_FALSE(b.book().crossed());
        }
    }
}

}  // namespace
}  // namespace ttt

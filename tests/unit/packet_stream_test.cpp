#include "ttt/replay/packet_stream.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "itch/core/hash.hpp"
#include "itch/wire/framing.hpp"
#include "support/itch_stream.hpp"
#include "ttt/core/rng.hpp"

namespace ttt::replay {
namespace {

using itch::wire::FrameCursor;
using Bytes = std::vector<std::byte>;

struct Out {
    Emitted e;
    Bytes   bytes;
};

std::vector<Out> drain(PacketStream<FrameCursor>& s) {
    std::vector<Out> out;
    Emitted          e;
    for (;;) {
        const StreamStatus st = s.next(e);
        if (st == StreamStatus::Finished) {
            break;
        }
        EXPECT_EQ(st, StreamStatus::Ok);
        if (st != StreamStatus::Ok) {
            break;
        }
        out.push_back({e, Bytes(e.bytes.begin(), e.bytes.end())});
    }
    return out;
}

u64 ts_of(std::span<const std::byte> m) { return itch::load_be48(m.data() + 5); }

// The invariants every stream must satisfy, whatever the configuration:
// every input message comes out exactly once and in order, sequence numbers are
// contiguous from the first, packets respect the budget and cap, send times
// never go backwards, and the stream ends with exactly one end of session that
// announces the next sequence number.
void check_common(const std::vector<Out>& out, const Bytes& input, const StreamConfig& cfg) {
    ASSERT_FALSE(out.empty());
    u64         expect_seq = cfg.first_sequence;
    u64         prev_send = 0;
    std::size_t in_pos = 0;

    for (std::size_t i = 0; i < out.size(); ++i) {
        const Out& o = out[i];
        ASSERT_GE(o.e.send_ns, prev_send) << "packet " << i;
        prev_send = o.e.send_ns;

        mold::PacketView v;
        ASSERT_EQ(mold::decode(o.bytes, v), mold::DecodeError::None);
        ASSERT_EQ(v.header().kind(), o.e.kind);
        ASSERT_EQ(v.header().sequence, expect_seq);
        ASSERT_EQ(v.header().session, cfg.session);
        ASSERT_LE(o.bytes.size(), cfg.budget);

        if (o.e.kind == mold::Kind::EndOfSession) {
            ASSERT_EQ(i, out.size() - 1) << "end of session must be last";
            continue;
        }
        if (o.e.kind == mold::Kind::Heartbeat) {
            continue;
        }
        ASSERT_GT(o.e.count, 0u);
        if (cfg.max_messages_per_packet != 0) {
            ASSERT_LE(o.e.count, cfg.max_messages_per_packet);
        }
        for (auto m : v) {
            // Same bytes as the framed input, in order.
            const std::size_t len = itch::load_be<u16>(input.data() + in_pos);
            ASSERT_EQ(m.size(), len);
            ASSERT_TRUE(
                std::equal(m.begin(), m.end(), input.begin() + static_cast<long>(in_pos + 2)));
            in_pos += 2 + len;
        }
        expect_seq += o.e.count;
    }
    ASSERT_EQ(in_pos, input.size()) << "not every message was sent";
    ASSERT_EQ(out.back().e.kind, mold::Kind::EndOfSession);
}

TEST(PacketStream, FixedRateScheduleIsExact) {
    Rng         rng(1);
    const auto  msgs = test::mixed_messages(rng, 5000, 1000, 0);
    const Bytes input = test::framed(msgs);

    StreamConfig cfg;
    cfg.session = mold::Session("FIXED");
    cfg.budget = 300;
    cfg.packets_per_second = 3;  // a rate that does not divide 1e9

    PacketStream<FrameCursor> s(FrameCursor(input), cfg);
    const auto                out = drain(s);
    check_common(out, input, cfg);

    for (std::size_t i = 0; i < out.size(); ++i) {
        const u64 want = static_cast<u64>(static_cast<u128>(i) * 1'000'000'000u / 3);
        ASSERT_EQ(out[i].e.send_ns, want) << "packet " << i;
    }
    // Greedy fill: only the last data packet may close for any reason but Full.
    EXPECT_EQ(s.stats().closed[static_cast<int>(CloseReason::EndOfInput)], 1u);
    EXPECT_EQ(s.stats().closed[static_cast<int>(CloseReason::Full)], s.stats().packets - 1);
    EXPECT_EQ(s.stats().heartbeats, 0u);
}

TEST(PacketStream, MessageCap) {
    Rng         rng(2);
    const auto  msgs = test::mixed_messages(rng, 1001, 1000, 0);
    const Bytes input = test::framed(msgs);

    StreamConfig cfg;
    cfg.session = mold::Session("CAP");
    cfg.max_messages_per_packet = 1;

    PacketStream<FrameCursor> s(FrameCursor(input), cfg);
    const auto                out = drain(s);
    check_common(out, input, cfg);
    EXPECT_EQ(s.stats().packets, 1001u);  // one message per packet
}

// Feed-time pacing, across many seeds and configurations: nothing is sent before
// its message's time, nothing waits longer than the flush window, and silences
// are filled with heartbeats spaced exactly one interval apart.
TEST(PacketStream, FeedTimeProperty) {
    for (u64 seed = 1; seed <= 300; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        Rng rng(seed);

        StreamConfig cfg;
        cfg.session = mold::Session("FEED");
        cfg.pacing = Pacing::FeedTime;
        cfg.budget = rng.range(72, mold::kMaxPayloadStandardMtu);
        cfg.speed = rng.range(1, 4);
        cfg.flush_window_ns = rng.range(0, 3) * 50'000;
        cfg.heartbeat_interval_ns = rng.range(0, 1) * 10'000'000;
        cfg.max_messages_per_packet = static_cast<u16>(rng.range(0, 1) * rng.range(1, 20));
        cfg.first_sequence = rng.range(1, 1000);

        const auto  msgs = test::mixed_messages(rng, rng.range(1, 4000), 40'000, 200'000'000);
        const Bytes input = test::framed(msgs);

        PacketStream<FrameCursor> s(FrameCursor(input), cfg);
        const auto                out = drain(s);
        check_common(out, input, cfg);

        const u64 t0 = ts_of(msgs.front().bytes());
        u64       last_send = 0;
        for (const Out& o : out) {
            if (o.e.kind == mold::Kind::Heartbeat) {
                ASSERT_NE(cfg.heartbeat_interval_ns, 0u);
                ASSERT_EQ(o.e.send_ns, last_send + cfg.heartbeat_interval_ns);
            } else if (o.e.kind == mold::Kind::Messages) {
                mold::PacketView v;
                ASSERT_EQ(mold::decode(o.bytes, v), mold::DecodeError::None);
                for (auto m : v) {
                    const u64 due = (ts_of(m) - t0) / cfg.speed;
                    ASSERT_GE(o.e.send_ns, due);
                    ASSERT_LE(o.e.send_ns, due + cfg.flush_window_ns);
                }
                if (cfg.heartbeat_interval_ns != 0 && last_send != 0) {
                    // No silence longer than one interval went unfilled.
                    ASSERT_LE(o.e.send_ns - last_send,
                              cfg.heartbeat_interval_ns + cfg.flush_window_ns);
                }
            }
            last_send = o.e.send_ns;
        }
        EXPECT_EQ(s.stats().messages, msgs.size());
    }
}

TEST(PacketStream, TimestampsGoingBackwardsAreClamped) {
    std::vector<itch_out::Message> msgs;
    msgs.push_back(itch_out::order_delete({1, 0, 1000}, 1));
    msgs.push_back(itch_out::order_delete({1, 0, 900}, 2));  // backwards
    msgs.push_back(itch_out::order_delete({1, 0, 2000}, 3));
    const Bytes input = test::framed(msgs);

    StreamConfig cfg;
    cfg.session = mold::Session("CLAMP");
    cfg.pacing = Pacing::FeedTime;
    cfg.max_messages_per_packet = 1;

    PacketStream<FrameCursor> s(FrameCursor(input), cfg);
    const auto                out = drain(s);
    check_common(out, input, cfg);
    EXPECT_EQ(s.stats().clamped_timestamps, 1u);
    EXPECT_EQ(out[0].e.send_ns, 0u);
    EXPECT_EQ(out[1].e.send_ns, 0u);
    EXPECT_EQ(out[2].e.send_ns, 1000u);
}

TEST(PacketStream, EmptyInputIsJustEndOfSession) {
    const Bytes               input;
    StreamConfig              cfg;
    PacketStream<FrameCursor> s(FrameCursor(input), cfg);
    const auto                out = drain(s);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].e.kind, mold::Kind::EndOfSession);
    EXPECT_EQ(out[0].e.sequence, 1u);
}

TEST(PacketStream, MalformedSourceStops) {
    Rng   rng(3);
    Bytes input = test::framed(test::mixed_messages(rng, 10, 1000, 0));
    input.push_back(std::byte{0});
    input.push_back(std::byte{3});
    input.push_back(std::byte{'Z'});  // unknown type
    input.push_back(std::byte{0});
    input.push_back(std::byte{0});

    StreamConfig              cfg;
    PacketStream<FrameCursor> s(FrameCursor(input), cfg);
    Emitted                   e;
    StreamStatus              st;
    while ((st = s.next(e)) == StreamStatus::Ok) {
    }
    EXPECT_EQ(st, StreamStatus::Malformed);
}

// The same input and configuration give byte-identical output, times included.
TEST(PacketStream, Deterministic) {
    Rng        rng(4);
    const auto input = test::framed(test::mixed_messages(rng, 3000, 100'000, 50'000'000));

    StreamConfig cfg;
    cfg.pacing = Pacing::FeedTime;
    cfg.flush_window_ns = 20'000;
    cfg.heartbeat_interval_ns = 5'000'000;

    auto digest = [&] {
        PacketStream<FrameCursor> s(FrameCursor(input), cfg);
        itch::Digest              d;
        for (const Out& o : drain(s)) {
            d.feed(o.e.send_ns);
            for (auto b : o.bytes) {
                d.feed(static_cast<u64>(b));
            }
        }
        return d.value();
    };
    EXPECT_EQ(digest(), digest());
}

}  // namespace
}  // namespace ttt::replay

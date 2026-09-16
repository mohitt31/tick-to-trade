#include "ttt/mold/mold.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "ttt/core/rng.hpp"
#include "ttt/mold/packer.hpp"

namespace ttt::mold {
namespace {

using Bytes = std::vector<std::byte>;

Bytes copy(std::span<const std::byte> s) { return Bytes(s.begin(), s.end()); }

Bytes random_message(Rng& rng, std::size_t max_len) {
    Bytes m(rng.range(1, max_len));
    for (auto& b : m) {
        b = static_cast<std::byte>(rng.next());
    }
    return m;
}

std::vector<Bytes> messages_of(const PacketView& v) {
    std::vector<Bytes> out;
    for (auto m : v) {
        out.push_back(copy(m));
    }
    return out;
}

TEST(Mold, HeaderRoundTrip) {
    Bytes        buf(kHeaderSize);
    const Header h{Session("20190130A"), 0x0102030405060708ULL, 7};
    store_header(buf, h);

    // Field positions straight from the specification.
    EXPECT_EQ(static_cast<char>(buf[0]), '2');
    EXPECT_EQ(static_cast<char>(buf[9]), ' ');  // space padded
    EXPECT_EQ(buf[10], std::byte{0x01});
    EXPECT_EQ(buf[17], std::byte{0x08});
    EXPECT_EQ(buf[18], std::byte{0x00});
    EXPECT_EQ(buf[19], std::byte{0x07});
}

TEST(Mold, HeartbeatAndEndOfSession) {
    Packer p(Session("S"), 42, kMaxPayloadStandardMtu);

    PacketView v;
    ASSERT_EQ(decode(p.heartbeat(), v), DecodeError::None);
    EXPECT_EQ(v.header().kind(), Kind::Heartbeat);
    EXPECT_EQ(v.header().sequence, 42u);
    EXPECT_EQ(v.header().end_sequence(), 42u);
    EXPECT_EQ(v.begin(), v.end());

    ASSERT_EQ(decode(p.end_of_session(), v), DecodeError::None);
    EXPECT_EQ(v.header().kind(), Kind::EndOfSession);
    EXPECT_EQ(v.header().message_count(), 0u);
    EXPECT_EQ(v.header().end_sequence(), 42u);
}

TEST(Mold, SequenceCountsMessagesNotPackets) {
    Packer      p(Session("S"), 1, kMaxPayloadStandardMtu);
    const Bytes m(10, std::byte{0xAB});

    p.add(m);
    p.add(m);
    p.add(m);
    PacketView v;
    ASSERT_EQ(decode(p.finish(), v), DecodeError::None);
    EXPECT_EQ(v.header().sequence, 1u);
    EXPECT_EQ(v.header().count, 3u);

    p.add(m);
    ASSERT_EQ(decode(p.finish(), v), DecodeError::None);
    EXPECT_EQ(v.header().sequence, 4u);

    // A heartbeat now announces the next number to be used.
    ASSERT_EQ(decode(p.heartbeat(), v), DecodeError::None);
    EXPECT_EQ(v.header().sequence, 5u);
}

// Any stream, any budget: decoding the packets gives back exactly the messages
// that went in, in order, with contiguous sequence numbers, every packet within
// budget, and no packet closed while the next message would still have fit.
TEST(Mold, PackDecodeProperty) {
    for (std::uint64_t seed = 1; seed <= 2000; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        Rng rng(seed);

        const std::size_t max_len = rng.range(1, 60);
        const std::size_t budget =
            rng.range(kHeaderSize + kBlockLengthSize + max_len, kMaxPayloadStandardMtu);
        const std::uint64_t first = rng.range(1, 1'000'000);
        Packer              p(Session("PROP"), first, budget);

        std::vector<Bytes> in(rng.range(0, 3000));
        for (auto& m : in) {
            m = random_message(rng, max_len);
        }

        std::vector<Bytes> out;
        std::uint64_t      expect_seq = first;

        auto check_packet = [&](std::span<const std::byte> pkt, const Bytes* next) {
            ASSERT_LE(pkt.size(), budget);
            PacketView v;
            ASSERT_EQ(decode(pkt, v), DecodeError::None);
            ASSERT_EQ(v.header().kind(), Kind::Messages);
            ASSERT_EQ(v.header().sequence, expect_seq);
            const auto got = messages_of(v);
            ASSERT_EQ(got.size(), v.header().count);
            expect_seq += got.size();
            out.insert(out.end(), got.begin(), got.end());
            if (next != nullptr) {
                // Greedy: the message that forced this packet closed did not fit.
                ASSERT_GT(pkt.size() + kBlockLengthSize + next->size(), budget);
            }
        };

        for (const auto& m : in) {
            if (!p.fits(m.size())) {
                check_packet(p.finish(), &m);
            }
            p.add(m);
        }
        if (!p.empty()) {
            check_packet(p.finish(), nullptr);
        }

        ASSERT_EQ(out, in);
        ASSERT_EQ(p.next_sequence(), first + in.size());
    }
}

Bytes sample_packet() {
    Packer p(Session("CUT"), 100, kMaxPayloadStandardMtu);
    Rng    rng(7);
    for (int i = 0; i < 5; ++i) {
        p.add(random_message(rng, 40));
    }
    return copy(p.finish());
}

// Every strict prefix of a valid packet is rejected, and never as a success.
TEST(Mold, EveryTruncationIsRejected) {
    const Bytes pkt = sample_packet();
    PacketView  v;
    ASSERT_EQ(decode(pkt, v), DecodeError::None);

    for (std::size_t n = 0; n < pkt.size(); ++n) {
        SCOPED_TRACE(testing::Message() << "prefix " << n);
        const DecodeError e = decode(std::span(pkt).first(n), v);
        if (n < kHeaderSize) {
            EXPECT_EQ(e, DecodeError::TooShort);
        } else {
            EXPECT_TRUE(e == DecodeError::TruncatedLength || e == DecodeError::TruncatedBlock)
                << to_string(e);
        }
    }
}

TEST(Mold, CountMustMatchBody) {
    Bytes      pkt = sample_packet();
    PacketView v;

    Bytes more = pkt;
    itch::store_be<u16>(more.data() + 18, 6);
    EXPECT_EQ(decode(more, v), DecodeError::TruncatedLength);

    Bytes fewer = pkt;
    itch::store_be<u16>(fewer.data() + 18, 4);
    EXPECT_EQ(decode(fewer, v), DecodeError::TrailingBytes);

    Bytes trailing = pkt;
    trailing.push_back(std::byte{0});
    EXPECT_EQ(decode(trailing, v), DecodeError::TrailingBytes);
}

TEST(Mold, MalformedSpecialsAndBlocks) {
    PacketView v;

    Bytes hb(kHeaderSize + 1);
    store_header(hb, Header{Session("X"), 5, kHeartbeatCount});
    EXPECT_EQ(decode(hb, v), DecodeError::SpecialWithBody);

    Bytes eos(kHeaderSize + 3);
    store_header(eos, Header{Session("X"), 5, kEndOfSessionCount});
    EXPECT_EQ(decode(eos, v), DecodeError::SpecialWithBody);

    Bytes empty_block(kHeaderSize + kBlockLengthSize);
    store_header(empty_block, Header{Session("X"), 5, 1});
    EXPECT_EQ(decode(empty_block, v), DecodeError::EmptyBlock);

    Bytes overflow(kHeaderSize + kBlockLengthSize + 1);
    store_header(overflow, Header{Session("X"), ~u64{0}, 1});
    itch::store_be<u16>(overflow.data() + kHeaderSize, 1);
    EXPECT_EQ(decode(overflow, v), DecodeError::SequenceOverflow);
}

// A failed decode leaves the output alone, so a caller that ignores the error
// cannot iterate a half-validated packet.
TEST(Mold, FailedDecodeDoesNotTouchOutput) {
    const Bytes good = sample_packet();
    PacketView  v;
    ASSERT_EQ(decode(good, v), DecodeError::None);

    Bytes bad = good;
    bad.pop_back();
    ASSERT_NE(decode(bad, v), DecodeError::None);
    EXPECT_EQ(v.header().sequence, 100u);
    EXPECT_EQ(messages_of(v).size(), 5u);
}

// Random bytes: never a crash, and whenever decode says yes the blocks it hands
// out exactly tile the packet.
TEST(Mold, RandomBytesFuzz) {
    Rng         rng(0xF00D);
    std::size_t accepted = 0;
    for (int i = 0; i < 200'000; ++i) {
        Bytes b(rng.range(0, 64));
        for (auto& x : b) {
            x = static_cast<std::byte>(rng.next());
        }
        // Bias toward small counts so some inputs actually pass.
        if (b.size() >= kHeaderSize) {
            itch::store_be<u16>(b.data() + 18, static_cast<u16>(rng.range(0, 3)));
            itch::store_be<u64>(b.data() + 10, rng.range(1, 1u << 20));
        }
        PacketView v;
        if (decode(b, v) != DecodeError::None) {
            continue;
        }
        ++accepted;
        std::size_t total = kHeaderSize;
        std::size_t n = 0;
        for (auto m : v) {
            ASSERT_GE(m.data(), b.data());
            ASSERT_LE(m.data() + m.size(), b.data() + b.size());
            total += kBlockLengthSize + m.size();
            ++n;
        }
        ASSERT_EQ(n, v.header().message_count());
        ASSERT_EQ(total, b.size());
    }
    EXPECT_GT(accepted, 0u);  // the fuzz reached the success path at all
}

}  // namespace
}  // namespace ttt::mold

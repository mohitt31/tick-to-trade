#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "itch/wire/framing.hpp"
#include "support/itch_stream.hpp"
#include "ttt/audit/sequence_audit.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/pcap/pcap.hpp"
#include "ttt/replay/packet_stream.hpp"

namespace ttt {
namespace {

using Bytes = std::vector<std::byte>;
using itch::u16;
using itch::u64;

std::string temp_path(const char* tag) {
    return std::string(testing::TempDir()) + "ttt_" + tag + "_" + std::to_string(::getpid()) +
           ".pcap";
}

pcap::Endpoint ep(const char* s) {
    pcap::Endpoint e;
    EXPECT_TRUE(pcap::parse_endpoint(s, e)) << s;
    return e;
}

struct Pkt {
    u64   ts;
    Bytes bytes;
};

std::vector<Pkt> stream_packets(u64 seed, std::size_t n, u16 cap) {
    Rng                  rng(seed);
    const Bytes          input = test::framed(test::mixed_messages(rng, n, 1000, 0));
    replay::StreamConfig cfg;
    cfg.session = mold::Session("AUDIT");
    cfg.max_messages_per_packet = cap;
    replay::PacketStream<itch::wire::FrameCursor> s(itch::wire::FrameCursor(input), cfg);
    std::vector<Pkt>                              out;
    replay::Emitted                               e;
    while (s.next(e) == replay::StreamStatus::Ok) {
        out.push_back({e.send_ns, Bytes(e.bytes.begin(), e.bytes.end())});
    }
    return out;
}

audit::FlowReport run_audit(const std::vector<Pkt>& pkts) {
    audit::SequenceAudit a;
    const auto           dst = ep("239.1.1.1:30001");
    for (const Pkt& p : pkts) {
        a.observe(p.ts, dst, p.bytes);
    }
    EXPECT_EQ(a.flows().size(), 1u);
    return a.flows().begin()->second;
}

TEST(Pcap, EndpointParsing) {
    pcap::Endpoint e;
    EXPECT_TRUE(pcap::parse_endpoint("239.255.0.7:30001", e));
    EXPECT_EQ(pcap::to_string(e), "239.255.0.7:30001");
    for (const char* bad : {"", "1.2.3:4", "1.2.3.4", "1.2.3.256:1", "1.2.3.4:65536", "1.2.3.4:x",
                            "1..3.4:1", "1.2.3.4:1 "}) {
        EXPECT_FALSE(pcap::parse_endpoint(bad, e)) << bad;
    }
}

TEST(Pcap, MulticastMac) {
    // 239.129.2.3: the top bit of the second octet is dropped.
    const auto m = pcap::multicast_mac({239, 129, 2, 3});
    EXPECT_EQ(m, (std::array<itch::u8, 6>{0x01, 0x00, 0x5e, 0x01, 0x02, 0x03}));
}

TEST(Pcap, ChecksumKnownVector) {
    // The IPv4 header example from RFC 1071 discussions: checksum is 0xb861.
    const std::array<std::byte, 20> h{
        std::byte{0x45}, std::byte{0x00}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x40}, std::byte{0x11},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xc0}, std::byte{0xa8}, std::byte{0x00},
        std::byte{0x01}, std::byte{0xc0}, std::byte{0xa8}, std::byte{0x00}, std::byte{0xc7}};
    EXPECT_EQ(pcap::ipv4_checksum(h), 0xb861);
}

// Replayer output through the file and back: every datagram identical, clean
// checksums, and the audit calls it pristine.
TEST(Pcap, WriteReadRoundTripIsPristine) {
    const auto  pkts = stream_packets(11, 20'000, 7);
    const auto  src = ep("10.0.0.1:40000");
    const auto  dst = ep("239.1.1.1:30001");
    std::string path = temp_path("roundtrip");
    {
        pcap::Writer w(path);
        for (const Pkt& p : pkts) {
            w.write(1'700'000'000'000'000'000ULL + p.ts, src, dst, p.bytes);
        }
    }

    pcap::Reader         r(path);
    audit::SequenceAudit a;
    pcap::Datagram       d;
    std::size_t          i = 0;
    while (r.next(d) == pcap::ReadStatus::Ok) {
        ASSERT_LT(i, pkts.size());
        EXPECT_EQ(d.ts_ns, 1'700'000'000'000'000'000ULL + pkts[i].ts);
        EXPECT_EQ(d.src, src);
        EXPECT_EQ(d.dst, dst);
        ASSERT_TRUE(std::equal(d.payload.begin(), d.payload.end(), pkts[i].bytes.begin(),
                               pkts[i].bytes.end()));
        a.observe(d.ts_ns, d.dst, d.payload);
        ++i;
    }
    EXPECT_EQ(i, pkts.size());
    EXPECT_TRUE(r.nanosecond());
    EXPECT_EQ(r.stats().bad_ip_checksum, 0u);
    EXPECT_EQ(r.stats().bad_udp_checksum, 0u);
    EXPECT_EQ(r.stats().not_ipv4_udp, 0u);
    const auto& rep = a.flows().at(dst);
    EXPECT_TRUE(rep.pristine()) << audit::describe(dst, rep);
    EXPECT_EQ(rep.messages, 20'000u);
    std::remove(path.c_str());
}

TEST(Pcap, CorruptedPayloadFailsUdpChecksum) {
    const auto  pkts = stream_packets(12, 50, 5);
    std::string path = temp_path("corrupt");
    {
        pcap::Writer w(path);
        for (const Pkt& p : pkts) {
            w.write(p.ts, ep("10.0.0.1:1"), ep("239.1.1.1:2"), p.bytes);
        }
    }
    // Flip one payload byte of the first record: 24 file + 16 record + 42 headers.
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 24 + 16 + 42 + 25, SEEK_SET);
        const int c = std::fgetc(f);
        std::fseek(f, 24 + 16 + 42 + 25, SEEK_SET);
        std::fputc(c ^ 0x5A, f);
        std::fclose(f);
    }
    pcap::Reader   r(path);
    pcap::Datagram d;
    while (r.next(d) == pcap::ReadStatus::Ok) {
    }
    EXPECT_EQ(r.stats().bad_udp_checksum, 1u);
    std::remove(path.c_str());
}

TEST(Audit, DetectsADroppedPacket) {
    auto pkts = stream_packets(21, 1000, 10);
    pkts.erase(pkts.begin() + 30);  // messages 301..310
    const auto r = run_audit(pkts);
    EXPECT_FALSE(r.pristine());
    EXPECT_EQ(r.gaps_opened, 1u);
    EXPECT_EQ(r.missing_messages(), 10u);
    ASSERT_EQ(r.first_gaps.size(), 1u);
    EXPECT_EQ(r.first_gaps[0], (audit::Range{301, 311}));
}

TEST(Audit, DetectsADuplicate) {
    auto pkts = stream_packets(22, 1000, 10);
    pkts.insert(pkts.begin() + 50, pkts[20]);
    const auto r = run_audit(pkts);
    EXPECT_FALSE(r.pristine());
    EXPECT_EQ(r.duplicate_packets, 1u);
    EXPECT_EQ(r.missing_messages(), 0u);
    EXPECT_EQ(r.messages, 1000u);
}

TEST(Audit, ReorderIsLateNotLost) {
    auto pkts = stream_packets(23, 1000, 10);
    std::swap(pkts[40], pkts[41]);
    const auto r = run_audit(pkts);
    EXPECT_FALSE(r.pristine());
    EXPECT_EQ(r.gaps_opened, 1u);
    EXPECT_EQ(r.late_packets, 1u);
    EXPECT_EQ(r.missing_messages(), 0u);  // the hole was filled
    EXPECT_EQ(r.messages, 1000u);
}

// Losing the last data packet is only visible because the end of session (or a
// heartbeat) announces the next sequence number.
TEST(Audit, TailLossIsCaughtByControlPacket) {
    auto pkts = stream_packets(24, 1000, 10);
    ASSERT_GE(pkts.size(), 2u);
    pkts.erase(pkts.end() - 2);  // last data packet; end of session stays
    const auto r = run_audit(pkts);
    EXPECT_EQ(r.missing_messages(), 10u);
    EXPECT_EQ(r.end_of_session, 1u);
}

TEST(Audit, OverlappingRepacketization) {
    // Messages 1..10 then 6..15: the second packet overlaps the first.
    auto make = [](u64 first, u16 n) {
        Bytes b(mold::kHeaderSize + n * 3u);
        mold::store_header(b, mold::Header{mold::Session("AUDIT"), first, n});
        for (u16 i = 0; i < n; ++i) {
            itch::store_be<u16>(b.data() + mold::kHeaderSize + i * 3u, 1);
        }
        return b;
    };
    const auto r = run_audit({{0, make(1, 10)}, {1, make(6, 10)}});
    EXPECT_EQ(r.overlapping_packets, 1u);
    EXPECT_EQ(r.messages, 15u);
    EXPECT_EQ(r.next_expected, 16u);
}

// Random faults against a model: whatever mix of drops, duplicates and swaps is
// applied, the messages the audit counts plus the ones it reports missing equal
// the messages that were sent.
TEST(Audit, AccountingProperty) {
    for (u64 seed = 1; seed <= 200; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        Rng  rng(seed);
        auto pkts =
            stream_packets(seed * 7, rng.range(10, 2000), static_cast<u16>(rng.range(1, 12)));
        const auto sent_eos = pkts.back();
        pkts.pop_back();

        std::vector<Pkt> faulty;
        for (const Pkt& p : pkts) {
            if (rng.chance_ppm(100'000)) {
                continue;  // drop
            }
            faulty.push_back(p);
            if (rng.chance_ppm(50'000)) {
                faulty.push_back(p);  // duplicate
            }
        }
        for (std::size_t i = 1; i < faulty.size(); ++i) {
            if (rng.chance_ppm(50'000)) {
                std::swap(faulty[i - 1], faulty[i]);
            }
        }
        faulty.push_back(sent_eos);

        mold::PacketView v;
        ASSERT_EQ(mold::decode(sent_eos.bytes, v), mold::DecodeError::None);
        const u64 total = v.header().sequence - 1;

        const auto r = run_audit(faulty);
        ASSERT_EQ(r.messages + r.missing_messages(), total);
        ASSERT_EQ(r.next_expected, total + 1);
    }
}

}  // namespace
}  // namespace ttt

namespace ttt {
namespace {

TEST(Audit, HeadLossAndHeadReorder) {
    auto pkts = stream_packets(31, 100, 10);
    auto lost = pkts;
    lost.erase(lost.begin());  // messages 1..10 never arrive
    auto r = run_audit(lost);
    EXPECT_EQ(r.missing_messages(), 10u);
    ASSERT_EQ(r.first_gaps.size(), 1u);
    EXPECT_EQ(r.first_gaps[0], (audit::Range{1, 11}));

    auto swapped = pkts;
    std::swap(swapped[0], swapped[1]);
    r = run_audit(swapped);
    EXPECT_EQ(r.missing_messages(), 0u);
    EXPECT_EQ(r.late_packets, 1u);
    EXPECT_EQ(r.messages, 100u);
}

TEST(Audit, MidSessionStartsAtFirstSeen) {
    auto pkts = stream_packets(32, 100, 10);
    pkts.erase(pkts.begin(), pkts.begin() + 3);
    audit::SequenceAudit a(0);
    for (const Pkt& p : pkts) {
        a.observe(p.ts, ep("239.1.1.1:30001"), p.bytes);
    }
    const auto& r = a.flows().begin()->second;
    EXPECT_EQ(r.first_sequence, 31u);
    EXPECT_EQ(r.missing_messages(), 0u);
    EXPECT_TRUE(r.pristine());
}

}  // namespace
}  // namespace ttt

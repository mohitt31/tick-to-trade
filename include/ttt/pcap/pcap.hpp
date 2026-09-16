// Reads and writes pcap files of UDP datagrams.
//
// The replayer writes one so its output can be checked with tcpdump before any
// network is involved, and the audit reads both that file and captures taken off
// the real wire on Linux. Those are the same format, so one checker covers the
// replayer's intent and what actually arrived.
//
// Written files use nanosecond timestamps, Ethernet framing, and correct IPv4
// and UDP checksums, so tcpdump -vv has nothing to complain about. The reader
// accepts microsecond or nanosecond files in either byte order, steps over
// 802.1Q tags, and counts (rather than fails on) frames that are not IPv4 UDP,
// because a real capture also contains IGMP joins and ARP.
#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/net/endpoint.hpp"

namespace ttt::pcap {

using itch::u16;
using itch::u32;
using itch::u64;
using itch::u8;

// Endpoints live with the socket code; pcap uses the same type.
using net::Endpoint;
using net::parse_endpoint;
using net::to_string;

// The Ethernet destination for an IPv4 multicast group: 01:00:5e followed by
// the low 23 bits of the group address.
[[nodiscard]] std::array<u8, 6> multicast_mac(const std::array<u8, 4>& group) noexcept;

// RFC 1071 one's complement checksum over an IPv4 header.
[[nodiscard]] u16 ipv4_checksum(std::span<const std::byte> header) noexcept;

// UDP checksum including the IPv4 pseudo header. segment is the UDP header plus
// payload, with the checksum field zero.
[[nodiscard]] u16 udp_checksum(const std::array<u8, 4>& src, const std::array<u8, 4>& dst,
                               std::span<const std::byte> segment) noexcept;

class Writer {
public:
    explicit Writer(const std::string& path);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void write(u64 ts_ns, const Endpoint& src, const Endpoint& dst,
               std::span<const std::byte> payload);

    [[nodiscard]] u64 records() const noexcept { return records_; }

private:
    std::FILE*             f_ = nullptr;
    std::vector<std::byte> frame_;
    u64                    records_ = 0;
};

struct Datagram {
    u64                        ts_ns = 0;
    Endpoint                   src{};
    Endpoint                   dst{};
    std::span<const std::byte> payload{};
};

enum class ReadStatus : u8 { Ok, End, Malformed };

struct ReaderStats {
    u64 records = 0;
    u64 udp = 0;
    u64 not_ipv4_udp = 0;  // ARP, IGMP, IPv6, TCP...
    u64 fragments = 0;     // IPv4 fragments; never expected on this feed
    u64 truncated = 0;     // captured length shorter than the frame
    u64 bad_ip_checksum = 0;
    u64 bad_udp_checksum = 0;  // checksum present and wrong
};

class Reader {
public:
    explicit Reader(const std::string& path);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Advances to the next well-formed IPv4 UDP datagram. The payload span is
    // valid until the next call.
    ReadStatus next(Datagram& out);

    [[nodiscard]] const ReaderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool               nanosecond() const noexcept { return nano_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    [[nodiscard]] u32 file_u32(const std::byte* p) const noexcept;

    std::FILE*             f_ = nullptr;
    bool                   swapped_ = false;
    bool                   nano_ = false;
    std::vector<std::byte> rec_;
    ReaderStats            stats_{};
    std::string            error_;
};

}  // namespace ttt::pcap

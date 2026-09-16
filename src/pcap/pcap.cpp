#include "ttt/pcap/pcap.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"

namespace ttt::pcap {

namespace {

constexpr u32 kMagicMicro = 0xA1B2C3D4;
constexpr u32 kMagicNano = 0xA1B23C4D;
constexpr u32 kLinkEthernet = 1;
constexpr u32 kSnapLen = 65535;

constexpr std::size_t kEthHeader = 14;
constexpr std::size_t kVlanTag = 4;
constexpr std::size_t kIpHeader = 20;
constexpr std::size_t kUdpHeader = 8;
constexpr u16         kEtherIpv4 = 0x0800;
constexpr u16         kEtherVlan = 0x8100;
constexpr u8          kProtoUdp = 17;

constexpr std::size_t kFileHeader = 24;
constexpr std::size_t kRecordHeader = 16;

// Records are written in the host's byte order, which is what every pcap tool
// expects from a file whose magic reads back unswapped.
void put_host32(std::byte* p, u32 v) noexcept { std::memcpy(p, &v, 4); }
void put_host16(std::byte* p, u16 v) noexcept { std::memcpy(p, &v, 2); }

u32 fold(u32 sum) noexcept {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

u32 sum_bytes(u32 sum, std::span<const std::byte> b) noexcept {
    std::size_t i = 0;
    for (; i + 1 < b.size(); i += 2) {
        sum += (static_cast<u32>(b[i]) << 8) | static_cast<u32>(b[i + 1]);
        sum = fold(sum);
    }
    if (i < b.size()) {
        sum += static_cast<u32>(b[i]) << 8;
        sum = fold(sum);
    }
    return sum;
}

}  // namespace

std::array<u8, 6> multicast_mac(const std::array<u8, 4>& g) noexcept {
    return {0x01, 0x00, 0x5e, static_cast<u8>(g[1] & 0x7F), g[2], g[3]};
}

u16 ipv4_checksum(std::span<const std::byte> header) noexcept {
    return static_cast<u16>(~sum_bytes(0, header) & 0xFFFF);
}

u16 udp_checksum(const std::array<u8, 4>& src, const std::array<u8, 4>& dst,
                 std::span<const std::byte> segment) noexcept {
    std::array<std::byte, 12> pseudo{};
    for (std::size_t i = 0; i < 4; ++i) {
        pseudo[i] = static_cast<std::byte>(src[i]);
        pseudo[4 + i] = static_cast<std::byte>(dst[i]);
    }
    pseudo[9] = static_cast<std::byte>(kProtoUdp);
    itch::store_be<u16>(pseudo.data() + 10, static_cast<u16>(segment.size()));
    u32       sum = sum_bytes(sum_bytes(0, pseudo), segment);
    const u16 c = static_cast<u16>(~sum & 0xFFFF);
    return c == 0 ? u16{0xFFFF} : c;  // zero on the wire means "no checksum"
}

// ---------------------------------------------------------------------------

Writer::Writer(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    if (f_ == nullptr) {
        throw std::runtime_error("cannot create " + path + ": " + std::strerror(errno));
    }
    std::array<std::byte, kFileHeader> h{};
    put_host32(h.data(), kMagicNano);
    put_host16(h.data() + 4, 2);  // version 2.4
    put_host16(h.data() + 6, 4);
    put_host32(h.data() + 16, kSnapLen);
    put_host32(h.data() + 20, kLinkEthernet);
    if (std::fwrite(h.data(), 1, h.size(), f_) != h.size()) {
        throw std::runtime_error("cannot write pcap header to " + path);
    }
    frame_.reserve(kRecordHeader + kEthHeader + kIpHeader + kUdpHeader + 1500);
}

Writer::~Writer() {
    if (f_ != nullptr) {
        std::fclose(f_);
    }
}

void Writer::write(u64 ts_ns, const Endpoint& src, const Endpoint& dst,
                   std::span<const std::byte> payload) {
    const std::size_t udp_len = kUdpHeader + payload.size();
    const std::size_t ip_len = kIpHeader + udp_len;
    const std::size_t frame_len = kEthHeader + ip_len;
    ITCH_ASSERT_MSG(ip_len <= 0xFFFF, "datagram too large for IPv4");

    frame_.assign(kRecordHeader + frame_len, std::byte{0});
    std::byte* r = frame_.data();
    put_host32(r, static_cast<u32>(ts_ns / 1'000'000'000u));
    put_host32(r + 4, static_cast<u32>(ts_ns % 1'000'000'000u));
    put_host32(r + 8, static_cast<u32>(frame_len));
    put_host32(r + 12, static_cast<u32>(frame_len));

    std::byte*              eth = r + kRecordHeader;
    const bool              multicast = dst.ip[0] >= 224 && dst.ip[0] <= 239;
    const std::array<u8, 6> dmac =
        multicast ? multicast_mac(dst.ip) : std::array<u8, 6>{0x02, 0, 0, 0, 0, 0x02};
    const std::array<u8, 6> smac{0x02, 0, 0, 0, 0, 0x01};  // locally administered
    for (std::size_t i = 0; i < 6; ++i) {
        eth[i] = static_cast<std::byte>(dmac[i]);
        eth[6 + i] = static_cast<std::byte>(smac[i]);
    }
    itch::store_be<u16>(eth + 12, kEtherIpv4);

    std::byte* ip = eth + kEthHeader;
    ip[0] = std::byte{0x45};  // version 4, 20-byte header
    itch::store_be<u16>(ip + 2, static_cast<u16>(ip_len));
    itch::store_be<u16>(ip + 4, static_cast<u16>(records_));  // identification
    itch::store_be<u16>(ip + 6, 0x4000);                      // don't fragment
    ip[8] = std::byte{multicast ? u8{32} : u8{64}};           // TTL
    ip[9] = static_cast<std::byte>(kProtoUdp);
    for (std::size_t i = 0; i < 4; ++i) {
        ip[12 + i] = static_cast<std::byte>(src.ip[i]);
        ip[16 + i] = static_cast<std::byte>(dst.ip[i]);
    }
    itch::store_be<u16>(ip + 10, ipv4_checksum(std::span<const std::byte>(ip, kIpHeader)));

    std::byte* udp = ip + kIpHeader;
    itch::store_be<u16>(udp, src.port);
    itch::store_be<u16>(udp + 2, dst.port);
    itch::store_be<u16>(udp + 4, static_cast<u16>(udp_len));
    if (!payload.empty()) {
        std::memcpy(udp + kUdpHeader, payload.data(), payload.size());
    }
    itch::store_be<u16>(udp + 6,
                        udp_checksum(src.ip, dst.ip, std::span<const std::byte>(udp, udp_len)));

    if (std::fwrite(frame_.data(), 1, frame_.size(), f_) != frame_.size()) {
        throw std::runtime_error("pcap write failed");
    }
    ++records_;
}

// ---------------------------------------------------------------------------

Reader::Reader(const std::string& path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (f_ == nullptr) {
        throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    }
    std::array<std::byte, kFileHeader> h{};
    if (std::fread(h.data(), 1, h.size(), f_) != h.size()) {
        throw std::runtime_error(path + ": too short for a pcap header");
    }
    u32 magic = 0;
    std::memcpy(&magic, h.data(), 4);
    if (magic == kMagicMicro || magic == kMagicNano) {
        swapped_ = false;
    } else if (itch::byteswap_uint(magic) == kMagicMicro ||
               itch::byteswap_uint(magic) == kMagicNano) {
        swapped_ = true;
        magic = itch::byteswap_uint(magic);
    } else {
        throw std::runtime_error(path + ": not a pcap file (pcapng is not supported)");
    }
    nano_ = magic == kMagicNano;
    if (file_u32(h.data() + 20) != kLinkEthernet) {
        throw std::runtime_error(path +
                                 ": link type is not Ethernet; capture on an interface, "
                                 "not 'any'");
    }
}

Reader::~Reader() {
    if (f_ != nullptr) {
        std::fclose(f_);
    }
}

u32 Reader::file_u32(const std::byte* p) const noexcept {
    u32 v = 0;
    std::memcpy(&v, p, 4);
    return swapped_ ? itch::byteswap_uint(v) : v;
}

ReadStatus Reader::next(Datagram& out) {
    for (;;) {
        std::array<std::byte, kRecordHeader> rh{};
        const std::size_t                    got = std::fread(rh.data(), 1, rh.size(), f_);
        if (got == 0) {
            return ReadStatus::End;
        }
        if (got != rh.size()) {
            error_ = "file ends inside a record header";
            return ReadStatus::Malformed;
        }
        const u64 sec = file_u32(rh.data());
        const u64 frac = file_u32(rh.data() + 4);
        const u32 incl = file_u32(rh.data() + 8);
        const u32 orig = file_u32(rh.data() + 12);
        if (incl > kSnapLen * 4u) {
            error_ = "record length is implausible";
            return ReadStatus::Malformed;
        }
        rec_.resize(incl);
        if (incl != 0 && std::fread(rec_.data(), 1, incl, f_) != incl) {
            error_ = "file ends inside a record";
            return ReadStatus::Malformed;
        }
        ++stats_.records;
        if (incl < orig) {
            ++stats_.truncated;
            continue;
        }

        std::span<const std::byte> f(rec_.data(), rec_.size());
        if (f.size() < kEthHeader) {
            ++stats_.not_ipv4_udp;
            continue;
        }
        std::size_t off = 12;
        u16         ether = itch::load_be<u16>(f.data() + off);
        off += 2;
        if (ether == kEtherVlan) {
            if (f.size() < kEthHeader + kVlanTag) {
                ++stats_.not_ipv4_udp;
                continue;
            }
            ether = itch::load_be<u16>(f.data() + off + 2);
            off += kVlanTag;
        }
        if (ether != kEtherIpv4 || f.size() < off + kIpHeader) {
            ++stats_.not_ipv4_udp;
            continue;
        }
        const std::byte*  ip = f.data() + off;
        const u8          vihl = static_cast<u8>(ip[0]);
        const std::size_t ihl = static_cast<std::size_t>(vihl & 0x0F) * 4;
        if ((vihl >> 4) != 4 || ihl < kIpHeader || f.size() < off + ihl) {
            ++stats_.not_ipv4_udp;
            continue;
        }
        if (static_cast<u8>(ip[9]) != kProtoUdp) {
            ++stats_.not_ipv4_udp;
            continue;
        }
        if (ipv4_checksum(std::span<const std::byte>(ip, ihl)) != 0) {
            ++stats_.bad_ip_checksum;
            continue;
        }
        const u16 frag = itch::load_be<u16>(ip + 6);
        if ((frag & 0x3FFF) != 0) {  // MF set or a non-zero offset
            ++stats_.fragments;
            continue;
        }
        const std::size_t ip_total = itch::load_be<u16>(ip + 2);
        if (ip_total < ihl + kUdpHeader || f.size() < off + ip_total) {
            ++stats_.not_ipv4_udp;
            continue;
        }
        const std::byte*  udp = ip + ihl;
        const std::size_t udp_len = itch::load_be<u16>(udp + 4);
        if (udp_len < kUdpHeader || udp_len > ip_total - ihl) {
            ++stats_.not_ipv4_udp;
            continue;
        }

        Datagram d;
        for (std::size_t i = 0; i < 4; ++i) {
            d.src.ip[i] = static_cast<u8>(ip[12 + i]);
            d.dst.ip[i] = static_cast<u8>(ip[16 + i]);
        }
        d.src.port = itch::load_be<u16>(udp);
        d.dst.port = itch::load_be<u16>(udp + 2);

        if (itch::load_be<u16>(udp + 6) != 0) {
            // A correct checksum re-summed over the whole segment folds to zero.
            std::array<std::byte, 12> pseudo{};
            for (std::size_t i = 0; i < 4; ++i) {
                pseudo[i] = static_cast<std::byte>(d.src.ip[i]);
                pseudo[4 + i] = static_cast<std::byte>(d.dst.ip[i]);
            }
            pseudo[9] = static_cast<std::byte>(kProtoUdp);
            itch::store_be<u16>(pseudo.data() + 10, static_cast<u16>(udp_len));
            const u32 s = sum_bytes(sum_bytes(0, pseudo), std::span<const std::byte>(udp, udp_len));
            if ((s & 0xFFFF) != 0xFFFF) {
                // Hardware checksum offload leaves outgoing checksums unfilled
                // in a capture taken on the sending host, so this is counted
                // but the datagram is still delivered.
                ++stats_.bad_udp_checksum;
            }
        }

        d.ts_ns = sec * 1'000'000'000u + (nano_ ? frac : frac * 1000u);
        d.payload = std::span<const std::byte>(udp + kUdpHeader, udp_len - kUdpHeader);
        ++stats_.udp;
        out = d;
        return ReadStatus::Ok;
    }
}

}  // namespace ttt::pcap

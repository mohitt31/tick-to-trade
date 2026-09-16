// A measurement trailer carried after the MoldUDP64 packet in a datagram.
//
// The sender stamps two times into every feed datagram: when the packet was
// meant to leave (the absolute schedule) and when it actually called send.
// A receiver on the same host reads the same clock, so it can split its latency
// into the sender's own lateness and the time from send to receive. Measuring
// from the intended time, not the actual one, is what keeps a sender that falls
// behind from making the receiver look faster: the coordinated omission guard.
//
// The trailer sits after the MoldUDP64 packet and a receiver in trailer mode
// strips it before decoding, so the codec stays strict. Only measurement runs
// use it. It is never on a feed a real MoldUDP64 receiver would read.
//
//   magic    4 bytes  "TTT1"
//   feed     2 bytes  0 = A, 1 = B
//   flags    2 bytes  reserved, zero
//   index    8 bytes  packet index on its feed
//   intended 8 bytes  monotonic ns
//   sent     8 bytes  monotonic ns
//
// All big-endian, like everything else on this wire.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"

namespace ttt::measure {

using itch::u16;
using itch::u32;
using itch::u64;

inline constexpr std::size_t kTrailerSize = 32;

// The largest MoldUDP64 packet that still fits a 1500-byte MTU with a trailer.
// Past it the kernel fragments the datagram, and an XDP program sees the UDP
// header only in the first fragment.
inline constexpr std::size_t kMaxPacketWithTrailer = 1472 - kTrailerSize;
inline constexpr u32         kTrailerMagic = 0x54545431;  // "TTT1"

struct Trailer {
    u16 feed = 0;
    u64 index = 0;
    u64 intended_ns = 0;
    u64 sent_ns = 0;

    friend bool operator==(const Trailer&, const Trailer&) = default;
};

inline void store_trailer(std::span<std::byte> out, const Trailer& t) noexcept {
    ITCH_ASSERT(out.size() >= kTrailerSize);
    itch::store_be<u32>(out.data(), kTrailerMagic);
    itch::store_be<u16>(out.data() + 4, t.feed);
    itch::store_be<u16>(out.data() + 6, 0);
    itch::store_be<u64>(out.data() + 8, t.index);
    itch::store_be<u64>(out.data() + 16, t.intended_ns);
    itch::store_be<u64>(out.data() + 24, t.sent_ns);
}

// Splits a datagram into packet and trailer. Nothing if the datagram does not
// end in a trailer.
[[nodiscard]] inline std::optional<Trailer> split_trailer(
    std::span<const std::byte>& datagram) noexcept {
    if (datagram.size() < kTrailerSize) {
        return std::nullopt;
    }
    const std::byte* p = datagram.data() + datagram.size() - kTrailerSize;
    if (itch::load_be<u32>(p) != kTrailerMagic) {
        return std::nullopt;
    }
    Trailer t;
    t.feed = itch::load_be<u16>(p + 4);
    t.index = itch::load_be<u64>(p + 8);
    t.intended_ns = itch::load_be<u64>(p + 16);
    t.sent_ns = itch::load_be<u64>(p + 24);
    datagram = datagram.first(datagram.size() - kTrailerSize);
    return t;
}

}  // namespace ttt::measure

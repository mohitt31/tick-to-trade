// MoldUDP64: how NASDAQ puts ITCH on multicast.
//
// A downstream packet is a 20-byte header followed by message blocks:
//
//   session      10 bytes  ASCII, space padded; names the feed's day
//   sequence      8 bytes  big-endian; the sequence number of the FIRST message
//   count         2 bytes  big-endian; how many message blocks follow
//   blocks        count x (2-byte big-endian length, then that many bytes)
//
// Sequence numbers count messages, not packets. A packet carrying messages
// 101..105 has sequence 101 and count 5, and the next packet on the feed starts
// at 106. Everything later in this project (arbitration, gap detection,
// retransmission) works on those message numbers, which is why the packet
// boundary is allowed to differ between feed A and feed B.
//
// Two special counts:
//   0       heartbeat. Carries the NEXT sequence number the sender will use,
//           which is how a receiver notices it lost the tail of a burst while
//           the feed has gone quiet.
//   0xFFFF  end of session. Also carries the next sequence number.
//
// A retransmission request is a header alone: sequence is the first message
// wanted, count is how many.
//
// The decoder is strict. A packet whose blocks do not exactly fill it is
// rejected whole, before any message is handed out, because applying the first
// half of a corrupt packet to a book is worse than dropping all of it: a drop is
// a gap the arbiter can recover, a half-applied packet is not.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"

namespace ttt::mold {

using itch::u16;
using itch::u64;
using itch::u8;

inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kBlockLengthSize = 2;

inline constexpr u16 kHeartbeatCount = 0;
inline constexpr u16 kEndOfSessionCount = 0xFFFF;

// The largest count that means "this many messages". 0xFFFF is reserved.
inline constexpr u16 kMaxMessageCount = 0xFFFE;

// A UDP payload that fits one standard Ethernet frame: 1500 MTU minus the
// 20-byte IPv4 and 8-byte UDP headers.
inline constexpr std::size_t kMaxPayloadStandardMtu = 1472;

class Session {
public:
    constexpr Session() noexcept { bytes_.fill(' '); }

    // Longer names are a programming error, not something to truncate quietly:
    // two sessions that differ only past byte ten would compare equal on the wire.
    explicit constexpr Session(std::string_view name) noexcept {
        ITCH_ASSERT_MSG(name.size() <= kSessionSize, "session name longer than 10 bytes");
        bytes_.fill(' ');
        for (std::size_t i = 0; i < name.size(); ++i) {
            bytes_[i] = name[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(bytes_.data(), bytes_.size());
    }

    friend constexpr bool operator==(const Session&, const Session&) = default;

private:
    std::array<char, kSessionSize> bytes_{};
};

enum class Kind : u8 {
    Messages,      // count in 1..0xFFFE
    Heartbeat,     // count 0
    EndOfSession,  // count 0xFFFF
};

struct Header {
    Session session{};
    u64     sequence = 0;
    u16     count = 0;

    [[nodiscard]] constexpr Kind kind() const noexcept {
        if (count == kHeartbeatCount) {
            return Kind::Heartbeat;
        }
        if (count == kEndOfSessionCount) {
            return Kind::EndOfSession;
        }
        return Kind::Messages;
    }

    // Number of message blocks that follow. Zero for both special kinds.
    [[nodiscard]] constexpr u16 message_count() const noexcept {
        return kind() == Kind::Messages ? count : u16{0};
    }

    // One past the last message this packet carries. For a heartbeat or end of
    // session that is simply the sequence field.
    [[nodiscard]] constexpr u64 end_sequence() const noexcept { return sequence + message_count(); }

    friend constexpr bool operator==(const Header&, const Header&) = default;
};

enum class DecodeError : u8 {
    None,
    TooShort,          // fewer than 20 bytes
    TruncatedLength,   // a block's length prefix runs past the end
    TruncatedBlock,    // a block's body runs past the end
    EmptyBlock,        // zero-length block; no ITCH message is empty
    TrailingBytes,     // bytes left over after `count` blocks
    SpecialWithBody,   // heartbeat or end of session followed by bytes
    SequenceOverflow,  // sequence + count wraps u64
};

[[nodiscard]] constexpr const char* to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::TooShort: return "shorter than a header";
        case DecodeError::TruncatedLength: return "block length prefix truncated";
        case DecodeError::TruncatedBlock: return "block body truncated";
        case DecodeError::EmptyBlock: return "zero-length block";
        case DecodeError::TrailingBytes: return "bytes after the last block";
        case DecodeError::SpecialWithBody: return "heartbeat or end of session with a body";
        case DecodeError::SequenceOverflow: return "sequence number overflow";
    }
    return "unknown";
}

// Writes a header. The buffer must have room for kHeaderSize bytes.
inline void store_header(std::span<std::byte> out, const Header& h) noexcept {
    ITCH_ASSERT(out.size() >= kHeaderSize);
    const std::string_view s = h.session.view();
    for (std::size_t i = 0; i < kSessionSize; ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    itch::store_be<u64>(out.data() + kSessionSize, h.sequence);
    itch::store_be<u16>(out.data() + kSessionSize + 8, h.count);
}

// A decoded packet. Only a PacketView that came out of decode() with
// DecodeError::None may be iterated; the iterator trusts the lengths because
// decode() already walked all of them.
class PacketView {
public:
    class Iterator {
    public:
        using value_type = std::span<const std::byte>;

        constexpr Iterator() noexcept = default;
        constexpr Iterator(const std::byte* p, u16 left) noexcept : p_(p), left_(left) {}

        [[nodiscard]] value_type operator*() const noexcept {
            const std::size_t len = itch::load_be<u16>(p_);
            return value_type(p_ + kBlockLengthSize, len);
        }

        Iterator& operator++() noexcept {
            const std::size_t len = itch::load_be<u16>(p_);
            p_ += kBlockLengthSize + len;
            --left_;
            return *this;
        }

        [[nodiscard]] constexpr bool operator==(const Iterator& o) const noexcept {
            return left_ == o.left_;
        }

    private:
        const std::byte* p_ = nullptr;
        u16              left_ = 0;
    };

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] Iterator      begin() const noexcept {
        return Iterator(body_, header_.message_count());
    }
    [[nodiscard]] Iterator end() const noexcept { return Iterator(); }

private:
    friend DecodeError decode(std::span<const std::byte>, PacketView&) noexcept;

    Header           header_{};
    const std::byte* body_ = nullptr;
};

// Validates the whole packet and, only if all of it is well formed, fills out.
// On error out is left untouched.
[[nodiscard]] inline DecodeError decode(std::span<const std::byte> in, PacketView& out) noexcept {
    if (in.size() < kHeaderSize) {
        return DecodeError::TooShort;
    }

    Header                         h;
    std::array<char, kSessionSize> name{};
    for (std::size_t i = 0; i < kSessionSize; ++i) {
        name[i] = static_cast<char>(in[i]);
    }
    h.session = Session(std::string_view(name.data(), name.size()));
    h.sequence = itch::load_be<u64>(in.data() + kSessionSize);
    h.count = itch::load_be<u16>(in.data() + kSessionSize + 8);

    const std::span<const std::byte> body = in.subspan(kHeaderSize);

    if (h.kind() != Kind::Messages) {
        if (!body.empty()) {
            return DecodeError::SpecialWithBody;
        }
    } else {
        if (h.sequence > ~u64{0} - h.count) {
            return DecodeError::SequenceOverflow;
        }
        std::size_t pos = 0;
        for (u16 i = 0; i < h.count; ++i) {
            if (body.size() - pos < kBlockLengthSize) {
                return DecodeError::TruncatedLength;
            }
            const std::size_t len = itch::load_be<u16>(body.data() + pos);
            if (len == 0) {
                return DecodeError::EmptyBlock;
            }
            pos += kBlockLengthSize;
            if (body.size() - pos < len) {
                return DecodeError::TruncatedBlock;
            }
            pos += len;
        }
        if (pos != body.size()) {
            return DecodeError::TrailingBytes;
        }
    }

    out.header_ = h;
    out.body_ = body.data();
    return DecodeError::None;
}

}  // namespace ttt::mold

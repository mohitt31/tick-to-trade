// The snapshot a receiver fetches when a gap is too old or too large to
// retransmit.
//
// A snapshot is one symbol's book, written as ITCH messages: the symbol's stock
// directory entry, then one Add Order for every resting order, level by level,
// each level's orders in queue order. That is the same idea as NASDAQ's GLIMPSE,
// which also describes a book in ITCH, only much smaller. The receiver rebuilds
// its book by feeding these through the same builder it uses for live data, so
// there is no second code path that could disagree about what a book is.
//
// Queue order inside a level is load-bearing: adding the orders back in that
// order is what restores time priority, and the chaos oracle compares priority,
// not just depth.
//
//   magic          8 bytes  "TTTSNAP1"
//   session       10 bytes
//   last_sequence  8 bytes  big-endian; the snapshot reflects every message up
//                           to and including this one
//   count          4 bytes  big-endian
//   messages       count x (2-byte big-endian length, then the ITCH message)
//
// Decoding is all or nothing, like the MoldUDP64 decoder: a truncated snapshot
// (a connection cut mid-transfer) is rejected before a single message is used.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/messages.hpp"
#include "ttt/mold/mold.hpp"

namespace ttt::feed {

using itch::u32;
using itch::u64;

inline constexpr std::string_view kSnapshotMagic = "TTTSNAP1";
inline constexpr std::size_t      kSnapshotHeaderSize = 8 + mold::kSessionSize + 8 + 4;

class SnapshotWriter {
public:
    SnapshotWriter(std::vector<std::byte>& out, const mold::Session& session, u64 last_sequence)
        : out_(out) {
        out_.assign(kSnapshotHeaderSize, std::byte{0});
        for (std::size_t i = 0; i < kSnapshotMagic.size(); ++i) {
            out_[i] = static_cast<std::byte>(kSnapshotMagic[i]);
        }
        const std::string_view s = session.view();
        for (std::size_t i = 0; i < mold::kSessionSize; ++i) {
            out_[8 + i] = static_cast<std::byte>(s[i]);
        }
        itch::store_be<u64>(out_.data() + 8 + mold::kSessionSize, last_sequence);
    }

    void add(std::span<const std::byte> msg) {
        ITCH_ASSERT(!msg.empty() && msg.size() <= itch::wire::kMaxMessageLength);
        const std::size_t at = out_.size();
        out_.resize(at + 2 + msg.size());
        itch::store_be<itch::u16>(out_.data() + at, static_cast<itch::u16>(msg.size()));
        for (std::size_t i = 0; i < msg.size(); ++i) {
            out_[at + 2 + i] = msg[i];
        }
        ++count_;
        itch::store_be<u32>(out_.data() + 8 + mold::kSessionSize + 8, count_);
    }

private:
    std::vector<std::byte>& out_;
    u32                     count_ = 0;
};

class SnapshotView {
public:
    [[nodiscard]] const mold::Session& session() const noexcept { return session_; }
    [[nodiscard]] u64                  last_sequence() const noexcept { return last_; }
    [[nodiscard]] u32                  count() const noexcept { return count_; }

    // Calls fn(message) for each message, in order. Only valid after a
    // successful decode, which has already walked every length.
    template <class Fn>
    void for_each(Fn&& fn) const {
        std::size_t pos = 0;
        for (u32 i = 0; i < count_; ++i) {
            const std::size_t len = itch::load_be<itch::u16>(body_.data() + pos);
            fn(body_.subspan(pos + 2, len));
            pos += 2 + len;
        }
    }

private:
    friend bool decode_snapshot(std::span<const std::byte>, SnapshotView&) noexcept;

    mold::Session              session_{};
    u64                        last_ = 0;
    u32                        count_ = 0;
    std::span<const std::byte> body_{};
};

// True only if the whole snapshot is well formed, with every message a known
// ITCH type at its declared length. On failure out is untouched.
[[nodiscard]] inline bool decode_snapshot(std::span<const std::byte> in,
                                          SnapshotView&              out) noexcept {
    if (in.size() < kSnapshotHeaderSize) {
        return false;
    }
    for (std::size_t i = 0; i < kSnapshotMagic.size(); ++i) {
        if (static_cast<char>(in[i]) != kSnapshotMagic[i]) {
            return false;
        }
    }
    std::array<char, mold::kSessionSize> name{};
    for (std::size_t i = 0; i < mold::kSessionSize; ++i) {
        name[i] = static_cast<char>(in[8 + i]);
    }
    const u64 last = itch::load_be<u64>(in.data() + 8 + mold::kSessionSize);
    const u32 count = itch::load_be<u32>(in.data() + 8 + mold::kSessionSize + 8);
    const std::span<const std::byte> body = in.subspan(kSnapshotHeaderSize);

    std::size_t pos = 0;
    for (u32 i = 0; i < count; ++i) {
        if (body.size() - pos < 2) {
            return false;
        }
        const std::size_t len = itch::load_be<itch::u16>(body.data() + pos);
        pos += 2;
        if (len == 0 || body.size() - pos < len) {
            return false;
        }
        if (itch::wire::message_length(static_cast<char>(body[pos])) != len) {
            return false;
        }
        pos += len;
    }
    if (pos != body.size()) {
        return false;
    }

    out.session_ = mold::Session(std::string_view(name.data(), name.size()));
    out.last_ = last;
    out.count_ = count;
    out.body_ = body;
    return true;
}

}  // namespace ttt::feed

// Messages stored by sequence number in a fixed, direct-mapped table.
//
// Two users with the same need: the receiver holds messages that arrived ahead
// of a gap, and the rewind server holds recent messages to answer
// retransmission requests. Both look messages up by sequence number, both want
// a hard memory bound, and neither may allocate once running.
//
// Sequence s lives in slot s & (capacity - 1). One slot is 64 bytes, a whole
// cache line: the sequence number, the length, and up to 50 bytes of message,
// which is the longest ITCH message. A lookup is one load and one compare.
//
// When two sequence numbers share a slot, the later write wins, and the slot
// always records which sequence it holds, so a lookup can never return the
// wrong message. For the receiver that means a table of capacity C keeps the
// newest C messages it was handed. For the rewind server it means the last C
// messages sent. Rejected: a window relative to the next expected sequence. It
// needs an overflow policy for everything outside the window, and during a
// snapshot, when the receiver's frontier is frozen, everything is outside it.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/messages.hpp"
#include "ttt/core/mutant.hpp"

namespace ttt::feed {

using itch::u16;
using itch::u32;
using itch::u64;

class SeqSlots {
public:
    static constexpr std::size_t kMaxLen = itch::wire::kMaxMessageLength;

    struct Slot {
        u64                            seq = 0;
        u16                            len = 0;  // 0: empty
        std::array<std::byte, kMaxLen> data{};
    };
    static_assert(sizeof(Slot) == 64, "one slot per cache line");

    struct Put {
        bool replaced = false;  // an occupied slot was overwritten
        u64  old_seq = 0;       // what it held
    };

    explicit SeqSlots(u32 capacity) : slots_(capacity), mask_(capacity - 1) {
        ITCH_ASSERT_MSG(capacity > 0 && std::has_single_bit(capacity),
                        "capacity must be a power of two");
    }

    Put put(u64 seq, std::span<const std::byte> msg) noexcept {
        ITCH_ASSERT_MSG(!msg.empty() && msg.size() <= kMaxLen, "message does not fit a slot");
        Slot&     s = slots_[seq & mask_];
        const Put r{s.len != 0 && s.seq != seq, s.seq};
        if (s.len == 0) {
            ++size_;
        }
        s.seq = seq;
        s.len = static_cast<u16>(msg.size());
        std::memcpy(s.data.data(), msg.data(), msg.size());
        return r;
    }

    // Empty span if seq is not held.
    [[nodiscard]] std::span<const std::byte> find(u64 seq) const noexcept {
        const Slot& s = slots_[seq & mask_];
        if (s.len != 0 && (s.seq == seq || TTT_MUTANT(SlotSeqUnchecked))) {
            return std::span<const std::byte>(s.data.data(), s.len);
        }
        return {};
    }

    bool erase(u64 seq) noexcept {
        Slot& s = slots_[seq & mask_];
        if (s.len == 0 || s.seq != seq) {
            return false;
        }
        s.len = 0;
        --size_;
        return true;
    }

    // Every held sequence number in [from, to) is present, none missing.
    [[nodiscard]] bool holds_all(u64 from, u64 to) const noexcept {
        for (u64 q = from; q < to; ++q) {
            if (find(q).empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] u32 size() const noexcept { return size_; }
    [[nodiscard]] u32 capacity() const noexcept { return mask_ + 1; }

private:
    std::vector<Slot> slots_;
    u32               mask_;
    u32               size_ = 0;
};

}  // namespace ttt::feed

// Builds MoldUDP64 packets from a stream of messages.
//
// The packer owns sequence numbering and nothing else. When a packet is sent is
// the replayer's decision (a byte budget, a flush deadline, a burst boundary),
// so the packer only answers "does this message still fit" and "close the
// packet now".
//
// The buffer is fixed at construction and reused for every packet, so building
// a packet never allocates. finish() hands out a span into that buffer, which is
// valid until the next add().
#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "ttt/mold/mold.hpp"

namespace ttt::mold {

class Packer {
public:
    // budget: the largest UDP payload a packet may have, header included.
    Packer(Session session, u64 first_sequence, std::size_t budget)
        : session_(session), next_seq_(first_sequence), buf_(budget) {
        ITCH_ASSERT_MSG(budget > kHeaderSize + kBlockLengthSize,
                        "packet budget cannot hold a single message");
        ITCH_ASSERT_MSG(first_sequence >= 1, "MoldUDP64 sequence numbers start at 1");
    }

    // Would a message of this length fit in the packet being built?
    [[nodiscard]] bool fits(std::size_t len) const noexcept {
        return count_ < kMaxMessageCount && used_ + kBlockLengthSize + len <= buf_.size();
    }

    // Largest message any packet from this packer can carry.
    [[nodiscard]] std::size_t max_message() const noexcept {
        return buf_.size() - kHeaderSize - kBlockLengthSize;
    }

    // Appends a message. The caller checks fits() first and finishes the packet
    // when it returns false; adding a message that does not fit is a bug.
    void add(std::span<const std::byte> msg) noexcept {
        ITCH_ASSERT_MSG(!msg.empty(), "MoldUDP64 blocks cannot be empty");
        ITCH_ASSERT_MSG(msg.size() <= 0xFFFF, "block longer than its length prefix allows");
        ITCH_ASSERT_MSG(fits(msg.size()), "message does not fit; finish() the packet first");
        itch::store_be<u16>(buf_.data() + used_, static_cast<u16>(msg.size()));
        std::memcpy(buf_.data() + used_ + kBlockLengthSize, msg.data(), msg.size());
        used_ += kBlockLengthSize + msg.size();
        ++count_;
    }

    [[nodiscard]] bool        empty() const noexcept { return count_ == 0; }
    [[nodiscard]] u16         pending_count() const noexcept { return count_; }
    [[nodiscard]] std::size_t pending_bytes() const noexcept { return used_; }

    // Sequence number the next message added will get.
    [[nodiscard]] u64 next_sequence() const noexcept { return next_seq_ + count_; }

    // Closes the packet being built and returns it. Must not be empty.
    [[nodiscard]] std::span<const std::byte> finish() noexcept {
        ITCH_ASSERT_MSG(count_ != 0, "finish() on an empty packet; use heartbeat()");
        store_header(buf_, Header{session_, next_seq_, count_});
        const std::span<const std::byte> out(buf_.data(), used_);
        next_seq_ += count_;
        count_ = 0;
        used_ = kHeaderSize;
        return out;
    }

    // A heartbeat carrying the next sequence number. Only valid between
    // packets, otherwise it would announce a number that pending messages own.
    [[nodiscard]] std::span<const std::byte> heartbeat() noexcept {
        return special(kHeartbeatCount);
    }

    [[nodiscard]] std::span<const std::byte> end_of_session() noexcept {
        return special(kEndOfSessionCount);
    }

private:
    std::span<const std::byte> special(u16 count) noexcept {
        ITCH_ASSERT_MSG(count_ == 0, "control packet while messages are pending");
        store_header(buf_, Header{session_, next_seq_, count});
        return std::span<const std::byte>(buf_.data(), kHeaderSize);
    }

    Session                session_;
    u64                    next_seq_;
    std::vector<std::byte> buf_;
    std::size_t            used_ = kHeaderSize;
    u16                    count_ = 0;
};

}  // namespace ttt::mold

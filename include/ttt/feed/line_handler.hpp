// A/B arbitration, gap detection and recovery for one MoldUDP64 line.
//
// Like the replayer, this is a state machine with no I/O and no clock. Packets,
// snapshots and timer expiries come in with the current time; everything it
// wants done goes out through an Actions object: apply a message to the book,
// ask for a retransmission, ask for a snapshot. The chaos harness drives it in
// simulated time and the Linux receiver will drive it from real sockets.
//
// Arbitration is first-wins at message level. Feed A and feed B carry the same
// messages, possibly split into different packets. Each message is applied the
// first time its sequence number is the next one expected, whichever feed it
// came on. A message ahead of that is held in a SeqSlots table. A message behind
// it is a duplicate and dropped.
//
// The frontier is one past the highest sequence number anything has revealed:
// a data packet, a heartbeat or an end of session. A gap is open whenever the
// frontier is ahead of the next expected message. Recovery escalates:
//
//   GapWait         give the other feed arb_wait_ns to fill it
//   Retransmitting  ask the rewind server for the first hole; retry on timeout,
//                   and start the attempt count over whenever the hole moved
//   Snapshotting    the gap is larger than a retransmission is allowed to be,
//                   or retries ran out: fetch a snapshot, retry on timeout
//
// The book is never wrong. It only ever holds the exact state after the last
// message applied: in-order messages move it forward one at a time, and a
// snapshot replaces it in one step and only when complete. While a gap is open
// the book is correct but behind, and stale() says so.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <span>

#include "itch/core/assert.hpp"
#include "itch/core/types.hpp"
#include "ttt/core/mutant.hpp"
#include "ttt/feed/seq_slots.hpp"
#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/mold/mold.hpp"

namespace ttt::feed {

using itch::u16;
using itch::u32;
using itch::u64;
using itch::u8;

enum class Source : u8 { A = 0, B = 1, Rewind = 2 };

enum class State : u8 { Live, GapWait, Retransmitting, Snapshotting };

inline constexpr u64 kNever = ~u64{0};

template <class T>
concept Actions = requires(T& a, u64 seq, u16 count, std::span<const std::byte> msg) {
    a.apply(seq, msg);      // the next message, in sequence order
    a.begin_snapshot(seq);  // throw the book away; the snapshot reflects up to seq
    a.apply_snapshot(msg);  // one message of the snapshot
    a.end_snapshot(seq);    // the book now reflects every message up to seq
    a.request_retransmit(seq, count);
    a.request_snapshot();
};

struct LineConfig {
    mold::Session session{};
    u64           arb_wait_ns = 200'000;
    u64           retransmit_timeout_ns = 5'000'000;
    u32           max_retransmit_attempts = 3;
    u64           max_retransmit_count = 1000;  // bigger gaps go straight to a snapshot
    u64           snapshot_timeout_ns = 100'000'000;
    u32           buffer_capacity = 1u << 14;  // power of two
};

struct LineStats {
    u64 packets[3] = {0, 0, 0};     // by Source
    u64 first_wins[3] = {0, 0, 0};  // messages applied straight off each source
    u64 decode_errors = 0;
    u64 wrong_session = 0;
    u64 applied = 0;
    u64 applied_from_buffer = 0;
    u64 duplicates = 0;
    u64 buffered = 0;
    u64 evicted = 0;  // a held message overwritten before it was used
    u64 gaps_opened = 0;
    u64 retransmit_requests = 0;
    u64 snapshot_requests = 0;
    u64 snapshots_applied = 0;
    u64 snapshots_ignored = 0;
    u64 snapshot_errors = 0;
};

template <Actions A>
class LineHandler {
public:
    LineHandler(const LineConfig& cfg, A& actions)
        : cfg_(cfg), a_(actions), slots_(cfg.buffer_capacity) {
        ITCH_ASSERT_MSG(
            cfg.max_retransmit_count >= 1 && cfg.max_retransmit_count <= mold::kMaxMessageCount,
            "retransmission size must fit a MoldUDP64 count");
        ITCH_ASSERT_MSG(cfg.max_retransmit_attempts >= 1, "at least one retransmission attempt");
    }

    void on_packet(Source src, std::span<const std::byte> bytes, u64 now) {
        ++stats_.packets[static_cast<u8>(src)];
        mold::PacketView v;
        if (mold::decode(bytes, v) != mold::DecodeError::None) {
            ++stats_.decode_errors;
            return;
        }
        const mold::Header& h = v.header();
        if (h.session != cfg_.session) {
            ++stats_.wrong_session;
            return;
        }

        if (h.kind() == mold::Kind::Messages) {
            u64 seq = h.sequence;
            if (TTT_MUTANT(OverlapMisnumbered) && h.sequence < expected_ &&
                h.end_sequence() > expected_) {
                seq = expected_;
            }
            for (auto m : v) {
                accept(src, seq, m);
                ++seq;
            }
            raise_frontier(h.end_sequence());
        } else {
            if (h.kind() == mold::Kind::EndOfSession) {
                ended_ = true;
            }
            if (!TTT_MUTANT(IgnoreControlFrontier)) {
                raise_frontier(h.sequence);
            }
        }
        settle(now);
    }

    void on_snapshot(std::span<const std::byte> bytes, u64 now) {
        SnapshotView sv;
        if (!decode_snapshot(bytes, sv) || sv.session() != cfg_.session) {
            ++stats_.snapshot_errors;
            return;
        }
        // Only a snapshot we are waiting for, and only one that does not take
        // the book backwards. A late answer to an earlier request is still a
        // correct book at its sequence number, so it is used if it is new enough.
        const u64 last = sv.last_sequence();
        if (state_ != State::Snapshotting || last + 1 < expected_) {
            ++stats_.snapshots_ignored;
            return;
        }

        a_.begin_snapshot(last);
        sv.for_each([&](std::span<const std::byte> m) { a_.apply_snapshot(m); });
        a_.end_snapshot(last);
        ++stats_.snapshots_applied;

        expected_ = last + 1;
        raise_frontier(expected_);
        drain();
        state_ = State::Live;
        deadline_ = kNever;
        settle(now);
    }

    void on_timer(u64 now) {
        if (now < deadline_) {
            return;
        }
        switch (state_) {
            case State::Live: deadline_ = kNever; break;
            case State::GapWait: retransmit_or_snapshot(now); break;
            case State::Retransmitting:
                if (expected_ != requested_from_) {
                    attempts_ = 0;  // the hole moved: progress, so a fresh budget
                }
                if (attempts_ >= cfg_.max_retransmit_attempts) {
                    start_snapshot(now);
                } else {
                    retransmit_or_snapshot(now);
                }
                break;
            case State::Snapshotting:
                a_.request_snapshot();
                ++stats_.snapshot_requests;
                deadline_ = now + cfg_.snapshot_timeout_ns;
                break;
        }
    }

    [[nodiscard]] u64              next_deadline() const noexcept { return deadline_; }
    [[nodiscard]] u64              next_expected() const noexcept { return expected_; }
    [[nodiscard]] u64              frontier() const noexcept { return frontier_; }
    [[nodiscard]] State            state() const noexcept { return state_; }
    [[nodiscard]] bool             stale() const noexcept { return state_ != State::Live; }
    [[nodiscard]] bool             ended() const noexcept { return ended_; }
    [[nodiscard]] const LineStats& stats() const noexcept { return stats_; }

private:
    void accept(Source src, u64 seq, std::span<const std::byte> m) {
        if (seq < expected_) {
            ++stats_.duplicates;
            if (!TTT_MUTANT(ApplyDuplicates)) {
                return;
            }
            a_.apply(seq, m);
            return;
        }
        if (seq == expected_) {
            a_.apply(seq, m);
            ++expected_;
            ++stats_.applied;
            ++stats_.first_wins[static_cast<u8>(src)];
            if (slots_.size() != 0) {
                drain();
            }
            return;
        }
        const SeqSlots::Put p = slots_.put(seq, m);
        ++stats_.buffered;
        if (p.replaced && p.old_seq >= expected_) {
            ++stats_.evicted;
        }
    }

    // Applies held messages for as long as the next one is there.
    void drain() {
        for (;;) {
            const std::span<const std::byte> m = slots_.find(expected_);
            if (m.empty()) {
                return;
            }
            a_.apply(expected_, m);
            slots_.erase(expected_);
            ++expected_;
            ++stats_.applied;
            ++stats_.applied_from_buffer;
        }
    }

    void raise_frontier(u64 next) noexcept { frontier_ = std::max(frontier_, next); }

    // Opens or closes the gap state after new input.
    void settle(u64 now) {
        ITCH_INVARIANT(frontier_ >= expected_);
        if (frontier_ == expected_) {
            if (state_ != State::Live) {
                state_ = State::Live;
                deadline_ = kNever;
            }
            return;
        }
        if (state_ == State::Live) {
            state_ = State::GapWait;
            attempts_ = 0;
            deadline_ = now + cfg_.arb_wait_ns;
            ++stats_.gaps_opened;
        }
    }

    void retransmit_or_snapshot(u64 now) {
        if (frontier_ - expected_ > cfg_.max_retransmit_count) {
            start_snapshot(now);
            return;
        }
        // Ask for the first hole only: from the next expected message up to the
        // first message already held, or to the frontier if none is.
        u64 end = expected_ + 1;
        while (end < frontier_ && slots_.find(end).empty()) {
            ++end;
        }
        const u64 count = std::min(end - expected_, cfg_.max_retransmit_count);
        state_ = State::Retransmitting;
        requested_from_ = expected_;
        ++attempts_;
        ++stats_.retransmit_requests;
        a_.request_retransmit(expected_, static_cast<u16>(count));
        deadline_ = now + cfg_.retransmit_timeout_ns;
    }

    void start_snapshot(u64 now) {
        state_ = State::Snapshotting;
        attempts_ = 0;
        ++stats_.snapshot_requests;
        a_.request_snapshot();
        deadline_ = now + cfg_.snapshot_timeout_ns;
    }

    LineConfig cfg_;
    A&         a_;
    SeqSlots   slots_;
    u64        expected_ = 1;  // a MoldUDP64 session starts at 1
    u64        frontier_ = 1;
    State      state_ = State::Live;
    u64        deadline_ = kNever;
    u64        requested_from_ = 0;
    u32        attempts_ = 0;
    bool       ended_ = false;
    LineStats  stats_{};
};

}  // namespace ttt::feed

// Turns a stream of ITCH messages into a timed stream of MoldUDP64 packets.
//
// This is the whole replayer except the socket. It decides what goes in each
// packet and when each packet is meant to leave, and it does both as a pure
// function of its input and configuration, with no clock. That is what lets the
// same code write a pcap on the Mac, drive the chaos harness in simulated time,
// and pace a real NIC on Linux, where the only added piece is a loop that waits
// for each send_ns and sends.
//
// Two pacing modes:
//
//   FixedRate  packets leave at exactly packets_per_second, filled greedily.
//              This is the measurement mode. The schedule is absolute:
//              packet i leaves at i * 1e9 / pps, computed from i rather than
//              accumulated, so rounding never drifts and a sender that falls
//              behind cannot quietly stretch the schedule. That is the
//              coordinated omission guard, and it starts here.
//
//   FeedTime   packets follow the ITCH timestamps, sped up by an integer
//              factor. A packet closes when the next message does not fit, or
//              when the next message comes later than the flush window allows,
//              which is how a real sender coalesces a burst. Silences longer
//              than the heartbeat interval are filled with heartbeats.
//
// The spans handed out are valid until the next call to next().
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "itch/core/assert.hpp"
#include "itch/core/endian.hpp"
#include "itch/core/types.hpp"
#include "itch/wire/framing.hpp"
#include "itch/wire/messages.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/mold/packer.hpp"

namespace ttt::replay {

using itch::u16;
using itch::u64;
using itch::u8;
using itch::wire::FrameStatus;

// Anything that yields framed ITCH messages the way Project 1's FrameCursor and
// FrameReader do.
template <class S>
concept MessageSource = requires(S& s, std::span<const std::byte>& out) {
    { s.next(out) } -> std::same_as<FrameStatus>;
};

enum class Pacing : u8 { FixedRate, FeedTime };

enum class CloseReason : u8 { Full, Window, Cap, EndOfInput };

struct StreamConfig {
    mold::Session session{};
    std::size_t   budget = mold::kMaxPayloadStandardMtu;
    u16           max_messages_per_packet = 0;  // 0: as many as fit
    u64           first_sequence = 1;
    Pacing        pacing = Pacing::FixedRate;

    // FixedRate
    u64 packets_per_second = 1000;

    // FeedTime
    u64 speed = 1;                              // feed time runs this many times faster
    u64 flush_window_ns = 0;                    // measured in replay time, after speed
    u64 heartbeat_interval_ns = 1'000'000'000;  // 0 disables heartbeats
};

struct Emitted {
    mold::Kind                 kind = mold::Kind::Messages;
    CloseReason                reason = CloseReason::Full;  // Messages only
    u64                        send_ns = 0;                 // intended, from stream start
    u64                        sequence = 0;
    u16                        count = 0;  // messages carried; 0 for control packets
    std::span<const std::byte> bytes{};
};

enum class StreamStatus : u8 {
    Ok,        // out holds a packet
    Finished,  // end of session already emitted; nothing more
    Malformed  // the source produced a frame it could not validate
};

struct StreamStats {
    u64  messages = 0;
    u64  packets = 0;
    u64  heartbeats = 0;
    u64  closed[4] = {0, 0, 0, 0};  // by CloseReason
    u64  clamped_timestamps = 0;    // ITCH timestamps that went backwards
    bool truncated_input = false;   // the source ended partway through a message
};

template <MessageSource Source>
class PacketStream {
public:
    PacketStream(Source source, const StreamConfig& cfg)
        : src_(std::move(source)),
          cfg_(cfg),
          packer_(cfg.session, cfg.first_sequence, cfg.budget),
          la_(itch::wire::kMaxMessageLength) {
        ITCH_ASSERT_MSG(packer_.max_message() >= itch::wire::kMaxMessageLength,
                        "packet budget cannot hold the longest ITCH message");
        ITCH_ASSERT_MSG(cfg.pacing != Pacing::FixedRate || cfg.packets_per_second > 0,
                        "fixed rate needs a rate");
        ITCH_ASSERT_MSG(cfg.pacing != Pacing::FeedTime || cfg.speed > 0, "feed time needs a speed");
    }

    StreamStatus next(Emitted& out) {
        if (eos_sent_) {
            return StreamStatus::Finished;
        }
        for (;;) {
            if (!have_la_ && !source_done_) {
                if (!pull()) {
                    return StreamStatus::Malformed;
                }
            }

            if (packer_.empty()) {
                if (!have_la_) {
                    emit_control(out, mold::Kind::EndOfSession, eos_time());
                    eos_sent_ = true;
                    return StreamStatus::Ok;
                }
                if (heartbeat_due()) {
                    emit_control(out, mold::Kind::Heartbeat,
                                 last_send_ns_ + cfg_.heartbeat_interval_ns);
                    return StreamStatus::Ok;
                }
                take_lookahead();
                continue;
            }

            if (!have_la_) {
                emit_packet(out, CloseReason::EndOfInput, window_close_time());
                return StreamStatus::Ok;
            }
            if (cfg_.max_messages_per_packet != 0 &&
                packer_.pending_count() == cfg_.max_messages_per_packet) {
                emit_packet(out, CloseReason::Cap, last_added_ns_);
                return StreamStatus::Ok;
            }
            if (!packer_.fits(la_len_)) {
                emit_packet(out, CloseReason::Full, last_added_ns_);
                return StreamStatus::Ok;
            }
            if (cfg_.pacing == Pacing::FeedTime &&
                la_ns_ > first_added_ns_ + cfg_.flush_window_ns) {
                emit_packet(out, CloseReason::Window, window_close_time());
                return StreamStatus::Ok;
            }
            take_lookahead();
        }
    }

    [[nodiscard]] const StreamStats& stats() const noexcept { return stats_; }
    [[nodiscard]] FrameStatus        source_status() const noexcept { return src_status_; }

private:
    // Reads one message into the lookahead. Returns false on a malformed frame.
    bool pull() {
        std::span<const std::byte> msg;
        src_status_ = src_.next(msg);
        switch (src_status_) {
            case FrameStatus::Ok: break;
            case FrameStatus::EndOfInput: source_done_ = true; return true;
            case FrameStatus::NeedMore:
                // Only a finished source reports NeedMore: the input ended in
                // the middle of a message.
                stats_.truncated_input = true;
                source_done_ = true;
                return true;
            case FrameStatus::Malformed: return false;
        }

        ITCH_ASSERT(msg.size() <= la_.size());
        std::memcpy(la_.data(), msg.data(), msg.size());
        la_len_ = msg.size();
        have_la_ = true;

        if (cfg_.pacing == Pacing::FeedTime) {
            ITCH_ASSERT_MSG(msg.size() >= offsetof(itch::wire::AddOrder, timestamp) + 6,
                            "frame too short to carry a timestamp");
            u64 ts = itch::load_be48(msg.data() + offsetof(itch::wire::AddOrder, timestamp));
            if (!seen_first_ts_) {
                first_ts_ = ts;
                prev_ts_ = ts;
                seen_first_ts_ = true;
            }
            if (ts < prev_ts_) {
                ++stats_.clamped_timestamps;
                ts = prev_ts_;
            }
            prev_ts_ = ts;
            la_ns_ = (ts - first_ts_) / cfg_.speed;
        }
        return true;
    }

    void take_lookahead() noexcept {
        if (packer_.empty()) {
            first_added_ns_ = la_ns_;
        }
        packer_.add(std::span<const std::byte>(la_.data(), la_len_));
        last_added_ns_ = la_ns_;
        have_la_ = false;
        ++stats_.messages;
    }

    [[nodiscard]] bool heartbeat_due() const noexcept {
        return cfg_.pacing == Pacing::FeedTime && cfg_.heartbeat_interval_ns != 0 && started_ &&
               la_ns_ > last_send_ns_ + cfg_.heartbeat_interval_ns;
    }

    // A packet that is not full leaves when its flush timer fires.
    [[nodiscard]] u64 window_close_time() const noexcept {
        return first_added_ns_ + cfg_.flush_window_ns;
    }

    [[nodiscard]] u64 fixed_slot(u64 index) const noexcept {
        return static_cast<u64>(static_cast<u128>(index) * 1'000'000'000u /
                                cfg_.packets_per_second);
    }

    [[nodiscard]] u64 eos_time() const noexcept {
        return cfg_.pacing == Pacing::FixedRate ? fixed_slot(slot_) : last_send_ns_;
    }

    void emit_packet(Emitted& out, CloseReason reason, u64 feed_time_send) noexcept {
        const u64 seq = packer_.next_sequence() - packer_.pending_count();
        const u16 n = packer_.pending_count();
        out.bytes = packer_.finish();
        out.kind = mold::Kind::Messages;
        out.reason = reason;
        out.sequence = seq;
        out.count = n;
        out.send_ns = stamp(cfg_.pacing == Pacing::FixedRate ? fixed_slot(slot_) : feed_time_send);
        ++slot_;
        ++stats_.packets;
        ++stats_.closed[static_cast<u8>(reason)];
    }

    void emit_control(Emitted& out, mold::Kind kind, u64 t) noexcept {
        out.bytes = kind == mold::Kind::Heartbeat ? packer_.heartbeat() : packer_.end_of_session();
        out.kind = kind;
        out.reason = CloseReason::EndOfInput;
        out.sequence = packer_.next_sequence();
        out.count = 0;
        out.send_ns = stamp(t);
        if (kind == mold::Kind::Heartbeat) {
            ++stats_.heartbeats;
        }
    }

    // Send times never go backwards.
    u64 stamp(u64 t) noexcept {
        t = std::max(t, last_send_ns_);
        last_send_ns_ = t;
        started_ = true;
        return t;
    }

    Source                 src_;
    StreamConfig           cfg_;
    mold::Packer           packer_;
    FrameStatus            src_status_ = FrameStatus::Ok;
    std::vector<std::byte> la_;
    std::size_t            la_len_ = 0;
    u64                    la_ns_ = 0;
    bool                   have_la_ = false;
    bool                   source_done_ = false;
    bool                   eos_sent_ = false;
    bool                   started_ = false;
    bool                   seen_first_ts_ = false;
    u64                    first_ts_ = 0;
    u64                    prev_ts_ = 0;
    u64                    first_added_ns_ = 0;
    u64                    last_added_ns_ = 0;
    u64                    last_send_ns_ = 0;
    u64                    slot_ = 0;
    StreamStats            stats_{};
};

}  // namespace ttt::replay

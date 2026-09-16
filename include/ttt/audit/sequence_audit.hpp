// Checks a stream of MoldUDP64 datagrams for exactly what a receiver cares about:
// is every message there, once, in order.
//
// This is the ground truth the NIC gate and every loss claim rest on. It trusts
// no counter anywhere in the stack; it only looks at sequence numbers. Packets
// are grouped by destination (one multicast group and port is one feed).
//
// Where the sequence starts matters. A MoldUDP64 session starts at 1, so by
// default anything before the first packet seen is a gap. Taking the first
// packet seen as the start instead would make losing the head of the session
// invisible, which is exactly the loss this is meant to catch. A capture that
// starts mid-session has to say so explicitly.
//
// A packet that arrives after the sequence has moved past it is classified by
// whether it fills a hole: if it does, it is a late (reordered) packet and the
// hole shrinks; if every message in it was already seen, it is a duplicate.
#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/pcap/pcap.hpp"

namespace ttt::audit {

using itch::u64;

struct Range {
    u64 from = 0;  // first missing sequence number
    u64 to = 0;    // one past the last

    friend bool operator==(const Range&, const Range&) = default;
};

struct FlowReport {
    u64 datagrams = 0;
    u64 data_packets = 0;
    u64 heartbeats = 0;
    u64 end_of_session = 0;
    u64 messages = 0;  // messages seen for the first time
    u64 decode_errors = 0;
    u64 session_changes = 0;
    u64 duplicate_packets = 0;    // every message already seen
    u64 overlapping_packets = 0;  // started before the frontier, ended past it
    u64 late_packets = 0;         // arrived behind the frontier and filled a hole
    u64 stale_control = 0;        // heartbeat or end of session behind the frontier
    u64 after_end = 0;            // data after end of session
    u64 time_regressions = 0;     // capture timestamp went backwards

    bool          started = false;
    mold::Session session{};
    u64           first_sequence = 0;
    u64           next_expected = 0;  // the frontier: one past the highest message seen
    u64           first_ts_ns = 0;
    u64           last_ts_ns = 0;

    // Holes still open, and every hole ever opened (first few only).
    std::map<u64, u64> open_gaps;
    u64                gaps_opened = 0;
    std::vector<Range> first_gaps;

    [[nodiscard]] u64 missing_messages() const noexcept {
        u64 n = 0;
        for (const auto& [from, to] : open_gaps) {
            n += to - from;
        }
        return n;
    }

    // Every message exactly once, in order, one session, properly ended.
    [[nodiscard]] bool pristine() const noexcept {
        return started && decode_errors == 0 && session_changes == 0 && gaps_opened == 0 &&
               duplicate_packets == 0 && overlapping_packets == 0 && late_packets == 0 &&
               stale_control == 0 && after_end == 0 && time_regressions == 0 && end_of_session > 0;
    }
};

class SequenceAudit {
public:
    // first_sequence: where every flow is expected to start. 0 means "wherever
    // the capture starts", for captures joined mid-session.
    explicit SequenceAudit(u64 first_sequence = 1) noexcept : first_sequence_(first_sequence) {}

    void observe(u64 ts_ns, const pcap::Endpoint& dst, std::span<const std::byte> payload);

    [[nodiscard]] const std::map<pcap::Endpoint, FlowReport>& flows() const noexcept {
        return flows_;
    }

private:
    u64                                  first_sequence_;
    std::map<pcap::Endpoint, FlowReport> flows_;
};

// Human-readable report for one flow.
[[nodiscard]] std::string describe(const pcap::Endpoint& dst, const FlowReport& r);

}  // namespace ttt::audit

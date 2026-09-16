#include "ttt/audit/sequence_audit.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>

#include "ttt/core/rng.hpp"

namespace ttt::audit {

namespace {

constexpr std::size_t kGapsKept = 10;

[[gnu::format(printf, 2, 3)]] void appendf(std::string& out, const char* fmt, ...) {
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out += line;
}

void open_gap(FlowReport& r, u64 from, u64 to) {
    r.open_gaps.emplace(from, to);
    ++r.gaps_opened;
    if (r.first_gaps.size() < kGapsKept) {
        r.first_gaps.push_back({from, to});
    }
}

// Removes [from, to) from the open holes. Returns how many missing messages it
// covered.
u64 fill(FlowReport& r, u64 from, u64 to) {
    u64  filled = 0;
    auto it = r.open_gaps.upper_bound(from);
    if (it != r.open_gaps.begin()) {
        --it;
    }
    while (it != r.open_gaps.end() && it->first < to) {
        const u64 g_from = it->first;
        const u64 g_to = it->second;
        if (g_to <= from) {
            ++it;
            continue;
        }
        const u64 lo = std::max(g_from, from);
        const u64 hi = std::min(g_to, to);
        filled += hi - lo;
        it = r.open_gaps.erase(it);
        if (g_from < lo) {
            r.open_gaps.emplace(g_from, lo);
        }
        if (hi < g_to) {
            it = r.open_gaps.emplace(hi, g_to).first;
            ++it;
        }
    }
    return filled;
}

}  // namespace

void SequenceAudit::observe(u64 ts_ns, const pcap::Endpoint& dst,
                            std::span<const std::byte> payload) {
    FlowReport& r = flows_[dst];
    ++r.datagrams;

    if (r.datagrams == 1) {
        r.first_ts_ns = ts_ns;
    } else if (ts_ns < r.last_ts_ns) {
        ++r.time_regressions;
    }
    r.last_ts_ns = std::max(r.last_ts_ns, ts_ns);

    mold::PacketView v;
    if (mold::decode(payload, v) != mold::DecodeError::None) {
        ++r.decode_errors;
        return;
    }
    const mold::Header& h = v.header();

    if (!r.started) {
        r.started = true;
        r.session = h.session;
        r.first_sequence = first_sequence_ != 0 ? first_sequence_ : h.sequence;
        r.next_expected = r.first_sequence;
    } else if (h.session != r.session) {
        ++r.session_changes;
    }

    const u64 s = h.sequence;
    const u64 e = h.end_sequence();

    if (h.kind() != mold::Kind::Messages) {
        if (h.kind() == mold::Kind::Heartbeat) {
            ++r.heartbeats;
        } else {
            ++r.end_of_session;
        }
        // A control packet announces the next number. If that is ahead of the
        // frontier, the tail of the data before it never arrived.
        if (s > r.next_expected) {
            open_gap(r, r.next_expected, s);
            r.next_expected = s;
        } else if (s < r.next_expected) {
            ++r.stale_control;
        }
        return;
    }

    ++r.data_packets;
    if (r.end_of_session > 0) {
        ++r.after_end;
    }

    if (s == r.next_expected) {
        r.messages += e - s;
        r.next_expected = e;
    } else if (s > r.next_expected) {
        open_gap(r, r.next_expected, s);
        r.messages += e - s;
        r.next_expected = e;
    } else if (e <= r.next_expected) {
        const u64 filled = fill(r, s, e);
        if (filled > 0) {
            ++r.late_packets;
            r.messages += filled;
        } else {
            ++r.duplicate_packets;
        }
    } else {
        // Starts behind the frontier, ends past it.
        ++r.overlapping_packets;
        r.messages += fill(r, s, r.next_expected) + (e - r.next_expected);
        r.next_expected = e;
    }
}

std::string describe(const pcap::Endpoint& dst, const FlowReport& r) {
    std::string out;

    appendf(out, "flow %s  session '%.*s'\n", pcap::to_string(dst).c_str(),
            static_cast<int>(r.session.view().size()), r.session.view().data());
    appendf(out,
            "  datagrams        %" PRIu64 "  (data %" PRIu64 ", heartbeat %" PRIu64
            ", end of session %" PRIu64 ")\n",
            r.datagrams, r.data_packets, r.heartbeats, r.end_of_session);
    appendf(out, "  sequence         %" PRIu64 " .. %" PRIu64 "\n", r.first_sequence,
            r.next_expected);
    appendf(out, "  messages         %" PRIu64 "\n", r.messages);
    appendf(out, "  missing          %" PRIu64 " in %zu open gaps (%" PRIu64 " ever opened)\n",
            r.missing_messages(), r.open_gaps.size(), r.gaps_opened);
    for (const Range& g : r.first_gaps) {
        appendf(out, "    gap            [%" PRIu64 ", %" PRIu64 ")\n", g.from, g.to);
    }
    appendf(out, "  duplicates       %" PRIu64 "  overlaps %" PRIu64 "  late %" PRIu64 "\n",
            r.duplicate_packets, r.overlapping_packets, r.late_packets);
    appendf(out,
            "  decode errors    %" PRIu64 "  session changes %" PRIu64 "  stale control %" PRIu64
            "  after end %" PRIu64 "  time regressions %" PRIu64 "\n",
            r.decode_errors, r.session_changes, r.stale_control, r.after_end, r.time_regressions);

    const u64 span_ns = r.last_ts_ns - r.first_ts_ns;
    if (r.datagrams > 1 && span_ns > 0) {
        // Rate over the capture: datagrams after the first, per second between
        // the first and last timestamp.
        const u64 milli_pps =
            static_cast<u64>((static_cast<u128>(r.datagrams - 1) * 1'000'000'000'000u) / span_ns);
        appendf(out,
                "  rate             %" PRIu64 ".%03" PRIu64 " datagrams/s over %" PRIu64 " ns\n",
                milli_pps / 1000, milli_pps % 1000, span_ns);
    }
    appendf(out, "  verdict          %s\n", r.pristine() ? "PRISTINE" : "NOT PRISTINE");
    return out;
}

}  // namespace ttt::audit

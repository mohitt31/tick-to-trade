// Audits a pcap of MoldUDP64 traffic: every message exactly once, in order.
//
// Works on the replayer's own output and on a capture taken off the wire, so the
// same check covers what was meant to be sent and what actually arrived.
//
// Usage:
//   ttt_pcap_audit [--mid-session] FILE.pcap
//
// By default every flow must start at sequence 1, as a MoldUDP64 session does.
// --mid-session takes the first packet seen as the start instead, for a capture
// joined partway through.
//
// Exit status 0 only if every flow is pristine.

#include <cinttypes>
#include <cstdio>
#include <exception>
#include <string_view>

#include "ttt/audit/sequence_audit.hpp"
#include "ttt/pcap/pcap.hpp"

using namespace ttt;

int main(int argc, char** argv) try {
    const bool mid = argc == 3 && std::string_view(argv[1]) == "--mid-session";
    if (argc != 2 && !mid) {
        std::fprintf(stderr, "usage: ttt_pcap_audit [--mid-session] FILE.pcap\n");
        return 2;
    }
    const char*          path = argv[argc - 1];
    pcap::Reader         reader(path);
    audit::SequenceAudit a(mid ? 0 : 1);
    pcap::Datagram       d;
    pcap::ReadStatus     st;
    while ((st = reader.next(d)) == pcap::ReadStatus::Ok) {
        a.observe(d.ts_ns, d.dst, d.payload);
    }
    const pcap::ReaderStats& rs = reader.stats();
    std::printf("file %s  (%s timestamps)\n", path, reader.nanosecond() ? "ns" : "us");
    std::printf("  records %" PRIu64 "  udp %" PRIu64 "  other %" PRIu64 "  fragments %" PRIu64
                "  truncated %" PRIu64 "  bad ip csum %" PRIu64 "  bad udp csum %" PRIu64 "\n",
                rs.records, rs.udp, rs.not_ipv4_udp, rs.fragments, rs.truncated, rs.bad_ip_checksum,
                rs.bad_udp_checksum);
    if (st == pcap::ReadStatus::Malformed) {
        std::printf("  file is malformed: %s\n", reader.error().c_str());
    }

    bool all = st != pcap::ReadStatus::Malformed && !a.flows().empty();
    for (const auto& [dst, report] : a.flows()) {
        std::fputs(audit::describe(dst, report).c_str(), stdout);
        all = all && report.pristine();
    }
    if (a.flows().empty()) {
        std::printf("no UDP datagrams found\n");
    }
    return all ? 0 : 1;
} catch (const std::exception& ex) {
    std::fprintf(stderr, "ttt_pcap_audit: %s\n", ex.what());
    return 2;
}

// The portable receiver: recvfrom on plain sockets, A/B arbitration, recovery,
// and the book.
//
// Usage:
//   ttt_recv [options]
//
//   --feed-a A.B.C.D:PORT      bind for feed A (default 239.1.1.1:30001, joined)
//   --feed-b A.B.C.D:PORT      bind for feed B (default 239.1.1.2:30001, joined)
//   --iface A.B.C.D            interface for multicast joins (default 127.0.0.1)
//   --rewind A.B.C.D:PORT      rewind server (default 127.0.0.1:31001)
//   --snapshot A.B.C.D:PORT    snapshot server (default 127.0.0.1:31002)
//   --session NAME             MoldUDP64 session (default TTTLIVE)
//   --symbol SYM               book to keep (default SYM1)
//   --arb-wait-ns N  --retransmit-timeout-ns N  --max-retransmit N  --snapshot-timeout-ns N
//   --idle-timeout-ms N        stop when nothing arrives this long (default 5000)
//   --expect-digest HEX        exit 1 unless the final book digest matches
//   --trailer                  feed datagrams carry the measurement trailer
//   --latency-out PREFIX       record latency (needs --trailer); writes PREFIX-<name>.hgrm,
//                              each headed by the machine manifest
//   --manifest-iface NAME      also describe this interface in the manifest (Linux)
//
// Latency needs the sender on the same host: both sides read one clock.
//
// Exit status 0 when end of session was seen, the book is complete, and the
// digest matches if one was given.

#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ttt/live/receiver.hpp"
#include "ttt/measure/manifest.hpp"

using namespace ttt;
using itch::u32;
using itch::u64;

namespace {

std::atomic<bool> g_stop{false};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "ttt_recv: %s\nsee the comment at the top of apps/recv.cpp\n",
                 why.c_str());
    std::exit(2);
}

u64 num(const std::string& s, int base = 10) {
    char*     end = nullptr;
    const u64 v = std::strtoull(s.c_str(), &end, base);
    if (s.empty() || *end != '\0') usage("bad number: " + s);
    return v;
}

net::Endpoint ep(const std::string& s) {
    net::Endpoint e;
    if (!net::parse_endpoint(s, e)) usage("bad endpoint: " + s);
    return e;
}

}  // namespace

int main(int argc, char** argv) try {
    live::ReceiverConfig cfg;
    cfg.line.session = mold::Session("TTTLIVE");
    cfg.feed[0] = ep("239.1.1.1:30001");
    cfg.feed[1] = ep("239.1.1.2:30001");
    cfg.rewind_server = ep("127.0.0.1:31001");
    cfg.snapshot_server = ep("127.0.0.1:31002");
    bool        check_digest = false;
    u64         expect = 0;
    std::string latency_out;
    std::string manifest_iface;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--feed-a") {
            cfg.feed[0] = ep(val());
        } else if (a == "--feed-b") {
            cfg.feed[1] = ep(val());
        } else if (a == "--iface") {
            cfg.iface = ep(val() + ":0").ip;
        } else if (a == "--rewind") {
            cfg.rewind_server = ep(val());
        } else if (a == "--snapshot") {
            cfg.snapshot_server = ep(val());
        } else if (a == "--session") {
            const std::string v = val();
            if (v.size() > mold::kSessionSize) usage("session longer than 10 chars");
            cfg.line.session = mold::Session(v);
        } else if (a == "--symbol") {
            cfg.symbol = val();
        } else if (a == "--arb-wait-ns") {
            cfg.line.arb_wait_ns = num(val());
        } else if (a == "--retransmit-timeout-ns") {
            cfg.line.retransmit_timeout_ns = num(val());
        } else if (a == "--max-retransmit") {
            cfg.line.max_retransmit_count = num(val());
            if (cfg.line.max_retransmit_count == 0 ||
                cfg.line.max_retransmit_count > mold::kMaxMessageCount) {
                usage("--max-retransmit must be in 1..65534");
            }
        } else if (a == "--snapshot-timeout-ns") {
            cfg.line.snapshot_timeout_ns = num(val());
        } else if (a == "--idle-timeout-ms") {
            cfg.idle_timeout_ns = num(val()) * 1'000'000;
        } else if (a == "--trailer") {
            cfg.trailer = true;
        } else if (a == "--latency-out") {
            latency_out = val();
            cfg.record_latency = true;
        } else if (a == "--manifest-iface") {
            manifest_iface = val();
        } else if (a == "--expect-digest") {
            expect = num(val(), 16);
            check_digest = true;
        } else {
            usage("unknown option " + a);
        }
    }

    if (cfg.record_latency && !cfg.trailer) usage("--latency-out needs --trailer");

    std::signal(SIGINT, [](int) { g_stop = true; });
    live::Receiver             rx(cfg);
    const live::ReceiverResult r = rx.run(g_stop);
    const auto&                l = r.line;

    std::printf("stopped: %s\n", r.stopped_because.c_str());
    std::printf("applied %" PRIu64 "  stale %s  ended %s\n", r.applied, r.stale ? "yes" : "no",
                r.ended ? "yes" : "no");
    std::printf("packets A %" PRIu64 " B %" PRIu64 " rewind %" PRIu64 "  first wins A %" PRIu64
                " B %" PRIu64 " rewind %" PRIu64 "\n",
                l.packets[0], l.packets[1], l.packets[2], l.first_wins[0], l.first_wins[1],
                l.first_wins[2]);
    std::printf("duplicates %" PRIu64 "  gaps %" PRIu64 "  retransmit requests %" PRIu64
                "  snapshots applied %" PRIu64 " (connects %" PRIu64 ", failed %" PRIu64 ")\n",
                l.duplicates, l.gaps_opened, l.retransmit_requests, l.snapshots_applied,
                r.snapshot_connects, r.snapshot_failures);
    std::printf("book digest %s %016" PRIx64 "\n", cfg.symbol.c_str(), r.digest);

    if (r.latency) {
        const measure::Manifest manifest = measure::collect_manifest(
            measure::ManifestOptions{manifest_iface, "ttt_recv recvfrom poll loop"});
        const std::pair<const char*, const measure::Histogram*> hists[] = {
            {"intended_to_recv", &r.latency->intended_to_recv},
            {"sent_to_recv", &r.latency->sent_to_recv},
            {"sender_lag", &r.latency->sender_lag},
            {"handler", &r.latency->handler},
        };
        for (const auto& [name, h] : hists) {
            std::printf("%-17s %s\n", name, h->summary().c_str());
            const std::string path = latency_out + "-" + name + ".hgrm";
            std::FILE*        f = std::fopen(path.c_str(), "w");
            if (f == nullptr) {
                std::fprintf(stderr, "ttt_recv: cannot write %s\n", path.c_str());
                return 1;
            }
            measure::write_manifest(f, manifest);
            std::fprintf(f, "# histogram: %s (values in microseconds)\n", name);
            h->write_hgrm(f);
            std::fclose(f);
        }
        std::printf("datagrams without a trailer: %" PRIu64 "\n", r.latency->without_trailer);
    }

    bool ok = r.ended && !r.stale;
    if (check_digest && r.digest != expect) {
        std::printf("digest MISMATCH, expected %016" PRIx64 "\n", expect);
        ok = false;
    }
    return ok ? 0 : 1;
} catch (const std::exception& ex) {
    std::fprintf(stderr, "ttt_recv: %s\n", ex.what());
    return 1;
}

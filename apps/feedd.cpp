// The feed server: feed A and B, rewind on UDP, snapshots on TCP.
//
// Usage:
//   ttt_feedd (--input FILE[.gz] | --generate SEED:MESSAGES) [options]
//
//   --feed-a A.B.C.D:PORT      feed A destination (default 239.1.1.1:30001)
//   --feed-b A.B.C.D:PORT      feed B destination (default 239.1.1.2:30001)
//   --iface A.B.C.D            interface for multicast (default 127.0.0.1)
//   --rewind A.B.C.D:PORT      rewind server bind (default 127.0.0.1:31001)
//   --snapshot A.B.C.D:PORT    snapshot server bind (default 127.0.0.1:31002)
//   --session NAME             MoldUDP64 session (default TTTLIVE)
//   --symbol SYM               book the snapshot server keeps (default SYM1)
//   --rate PPS                 fixed rate on both feeds (default 10000)
//   --feed-time SPEED          follow ITCH timestamps instead
//   --budget-a / --budget-b N  largest UDP payload per feed (default 1472)
//   --max-messages N           stop after N messages
//   --start-delay-ms N         wait before the first packet (default 500)
//   --faults SEED:DROP_PPM:DUP_PPM:JITTER_NS   on both feeds
//   --outage FROM:TO           drop sequence numbers [FROM, TO) on both feeds (repeatable)
//   --response-drop-ppm N      lose retransmission replies
//   --snapshot-cut-ppm N       close snapshot connections halfway
//   --trailer                  append the measurement trailer (intended and actual send time)
//   --busy-wait                spin between packets instead of sleeping (measurement pacing)
//   --preload                  build every packet in memory before the first send
//   --no-servers               no rewind or snapshot server; nothing on the send thread but sending
//
// A measurement sender is --trailer --busy-wait --preload --no-servers, pinned
// to its own core.
//
// Prints the snapshot server's final book digest; a receiver that got
// everything prints the same one.

#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "ttt/live/feed_server.hpp"
#include "ttt/replay/limited.hpp"
#include "ttt/sim/order_gen.hpp"

using namespace ttt;
using itch::u32;
using itch::u64;
using itch::wire::FdSource;
using itch::wire::FrameCursor;
using itch::wire::FrameReader;
using itch::wire::GzipSource;

namespace {

std::atomic<bool> g_stop{false};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "ttt_feedd: %s\nsee the comment at the top of apps/feedd.cpp\n",
                 why.c_str());
    std::exit(2);
}

u64 num(const std::string& s) {
    char*     end = nullptr;
    const u64 v = std::strtoull(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0') usage("bad number: " + s);
    return v;
}

net::Endpoint ep(const std::string& s) {
    net::Endpoint e;
    if (!net::parse_endpoint(s, e)) usage("bad endpoint: " + s);
    return e;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t              at = 0;
    for (;;) {
        const std::size_t p = s.find(sep, at);
        out.push_back(s.substr(at, p == std::string::npos ? std::string::npos : p - at));
        if (p == std::string::npos) return out;
        at = p + 1;
    }
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::unique_ptr<live::AnySource> file_source(const std::string& path, u64 limit) {
    if (ends_with(path, ".gz")) {
        using Gz = FrameReader<GzipSource<FdSource>>;
        return std::make_unique<live::OwnedSource<replay::Limited<Gz>>>(
            replay::Limited<Gz>(Gz(GzipSource<FdSource>(FdSource(path))), limit));
    }
    using Plain = FrameReader<FdSource>;
    return std::make_unique<live::OwnedSource<replay::Limited<Plain>>>(
        replay::Limited<Plain>(Plain(FdSource(path)), limit));
}

}  // namespace

int main(int argc, char** argv) try {
    live::FeedServerConfig cfg;
    cfg.feed[0] = ep("239.1.1.1:30001");
    cfg.feed[1] = ep("239.1.1.2:30001");
    cfg.rewind = ep("127.0.0.1:31001");
    cfg.snapshot = ep("127.0.0.1:31002");
    cfg.start_delay_ns = 500'000'000;
    for (auto& s : cfg.stream) {
        s.packets_per_second = 10'000;
    }
    std::string input;
    std::string generate;
    u64         max_messages = ~u64{0};

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--input") {
            input = val();
        } else if (a == "--generate") {
            generate = val();
        } else if (a == "--feed-a") {
            cfg.feed[0] = ep(val());
        } else if (a == "--feed-b") {
            cfg.feed[1] = ep(val());
        } else if (a == "--iface") {
            cfg.iface = ep(val() + ":0").ip;
        } else if (a == "--rewind") {
            cfg.rewind = ep(val());
        } else if (a == "--snapshot") {
            cfg.snapshot = ep(val());
        } else if (a == "--session") {
            const std::string v = val();
            if (v.size() > mold::kSessionSize) usage("session longer than 10 chars");
            cfg.session = mold::Session(v);
        } else if (a == "--symbol") {
            cfg.symbol = val();
        } else if (a == "--rate") {
            const u64 pps = num(val());
            if (pps == 0) usage("--rate must be positive");
            for (auto& s : cfg.stream) {
                s.pacing = replay::Pacing::FixedRate;
                s.packets_per_second = pps;
            }
        } else if (a == "--feed-time") {
            const u64 speed = num(val());
            if (speed == 0) usage("--feed-time must be positive");
            for (auto& s : cfg.stream) {
                s.pacing = replay::Pacing::FeedTime;
                s.speed = speed;
            }
        } else if (a == "--budget-a") {
            cfg.stream[0].budget = num(val());
        } else if (a == "--budget-b") {
            cfg.stream[1].budget = num(val());
        } else if (a == "--max-messages") {
            max_messages = num(val());
        } else if (a == "--start-delay-ms") {
            cfg.start_delay_ns = num(val()) * 1'000'000;
        } else if (a == "--faults") {
            const auto parts = split(val(), ':');
            if (parts.size() != 4) usage("--faults wants SEED:DROP_PPM:DUP_PPM:JITTER_NS");
            cfg.faults_on = true;
            cfg.faults.seed = num(parts[0]);
            for (auto& f : cfg.faults.feed) {
                f.drop_ppm = static_cast<u32>(num(parts[1]));
                f.dup_ppm = static_cast<u32>(num(parts[2]));
                f.jitter_ns = num(parts[3]);
            }
        } else if (a == "--outage") {
            const auto parts = split(val(), ':');
            if (parts.size() != 2) usage("--outage wants FROM:TO");
            cfg.faults_on = true;
            cfg.faults.joint_outages.emplace_back(num(parts[0]), num(parts[1]));
        } else if (a == "--response-drop-ppm") {
            cfg.faults_on = true;
            cfg.faults.response_drop_ppm = static_cast<u32>(num(val()));
        } else if (a == "--trailer") {
            cfg.trailer = true;
        } else if (a == "--busy-wait") {
            cfg.busy_wait = true;
        } else if (a == "--preload") {
            cfg.preload = true;
        } else if (a == "--no-servers") {
            cfg.servers = false;
        } else if (a == "--snapshot-cut-ppm") {
            cfg.faults_on = true;
            cfg.faults.snapshot_cut_ppm = static_cast<u32>(num(val()));
        } else {
            usage("unknown option " + a);
        }
    }
    if (input.empty() == generate.empty()) usage("exactly one of --input or --generate");
    for (const auto& s : cfg.stream) {
        if (s.budget < 72 || s.budget > 65507) usage("budget must be in 72..65507");
    }

    std::vector<std::byte>           framed;  // --generate keeps the stream in memory
    std::unique_ptr<live::AnySource> a;
    std::unique_ptr<live::AnySource> b;
    if (!generate.empty()) {
        const auto parts = split(generate, ':');
        if (parts.size() != 2) usage("--generate wants SEED:MESSAGES");
        Rng            rng(num(parts[0]));
        sim::GenConfig g;
        g.messages = static_cast<u32>(num(parts[1]));
        for (const auto& m : sim::generate(rng, g)) {
            const auto bytes = m.bytes();
            const auto at = framed.size();
            framed.resize(at + 2 + bytes.size());
            itch::store_be<itch::u16>(framed.data() + at, static_cast<itch::u16>(bytes.size()));
            std::copy(bytes.begin(), bytes.end(), framed.begin() + static_cast<long>(at + 2));
        }
        using Cur = replay::Limited<FrameCursor>;
        a = std::make_unique<live::OwnedSource<Cur>>(Cur(FrameCursor(framed), max_messages));
        b = std::make_unique<live::OwnedSource<Cur>>(Cur(FrameCursor(framed), max_messages));
    } else {
        a = file_source(input, max_messages);
        b = file_source(input, max_messages);
    }

    std::signal(SIGINT, [](int) { g_stop = true; });
    live::FeedServer server(cfg, std::move(a), std::move(b));
    std::printf("rewind %s  snapshot %s\n", net::to_string(server.rewind_endpoint()).c_str(),
                net::to_string(server.snapshot_endpoint()).c_str());
    std::fflush(stdout);

    const live::FeedServerStats s = server.run(g_stop);
    std::printf("messages %" PRIu64 "\n", s.messages);
    std::printf("sent A %" PRIu64 " B %" PRIu64 "  dropped A %" PRIu64 " B %" PRIu64
                "  duplicated A %" PRIu64 " B %" PRIu64 "  send errors %" PRIu64 "\n",
                s.sent[0], s.sent[1], s.dropped[0], s.dropped[1], s.duplicated[0], s.duplicated[1],
                s.send_errors);
    std::printf("rewind served %" PRIu64 " unavailable %" PRIu64 "  snapshots %" PRIu64
                " cut %" PRIu64 "\n",
                s.rewind_served, s.rewind_unavailable, s.snapshots_served, s.snapshots_cut);
    std::printf("max lag %" PRIu64 " ns\n", s.max_lag_ns);
    if (cfg.preload) {
        std::printf("preloaded %" PRIu64 " bytes\n", s.preloaded_bytes);
    }
    if (cfg.servers) {
        std::printf("book digest %s %016" PRIx64 "\n", cfg.symbol.c_str(), s.book_digest);
    } else {
        std::printf("book digest n/a (no servers)\n");
    }
    return 0;
} catch (const std::exception& ex) {
    std::fprintf(stderr, "ttt_feedd: %s\n", ex.what());
    return 1;
}

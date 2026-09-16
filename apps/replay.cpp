// Replays an ITCH file as MoldUDP64 packets into a pcap file.
//
// On the Mac this is the whole replayer's output, checkable with tcpdump and
// ttt_pcap_audit before any network exists. The Linux sender will reuse the same
// PacketStream and add only the wait-and-send loop.
//
// Usage:
//   ttt_replay --input FILE[.gz] --pcap OUT [options]
//
//   --group A.B.C.D:PORT     destination (default 239.1.1.1:30001)
//   --source A.B.C.D:PORT    source (default 10.0.0.1:40000)
//   --session NAME           MoldUDP64 session, up to 10 chars (default TTTSESSION)
//   --rate PPS               fixed-rate pacing (default 10000)
//   --feed-time SPEED        follow ITCH timestamps, SPEED times faster
//   --flush-window-ns N      feed time: longest a message waits in a packet (default 0)
//   --heartbeat-ns N         feed time: heartbeat after this much silence (default 1e9)
//   --budget BYTES           largest UDP payload (default 1472)
//   --max-per-packet N       cap on messages per packet (default: as many as fit)
//   --max-messages N         stop after N messages
//   --epoch-ns N             pcap time of the first packet (default 0)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "itch/wire/framing.hpp"
#include "itch/wire/gzip_source.hpp"
#include "itch/wire/source.hpp"
#include "ttt/pcap/pcap.hpp"
#include "ttt/replay/packet_stream.hpp"

using namespace ttt;
using itch::u16;
using itch::u64;
using itch::wire::FdSource;
using itch::wire::FrameReader;
using itch::wire::FrameStatus;
using itch::wire::GzipSource;

namespace {

// Stops a source after n messages, reporting a clean end of input.
template <replay::MessageSource Inner>
class Limited {
public:
    Limited(Inner inner, u64 limit) : inner_(std::move(inner)), left_(limit) {}

    FrameStatus next(std::span<const std::byte>& out) {
        if (left_ == 0) {
            return FrameStatus::EndOfInput;
        }
        const FrameStatus st = inner_.next(out);
        if (st == FrameStatus::Ok) {
            --left_;
        }
        return st;
    }

private:
    Inner inner_;
    u64   left_;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr, "ttt_replay: %s\nsee the comment at the top of apps/replay.cpp\n", why);
    std::exit(2);
}

u64 number(const char* flag, const char* text) {
    char*      end = nullptr;
    const auto v = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        usage((std::string("bad number for ") + flag).c_str());
    }
    return v;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

template <replay::MessageSource Source>
int run(Source src, const replay::StreamConfig& cfg, const std::string& out_path,
        const pcap::Endpoint& from, const pcap::Endpoint& to, u64 epoch_ns) {
    replay::PacketStream<Source> stream(std::move(src), cfg);
    pcap::Writer                 w(out_path);
    replay::Emitted              e;
    replay::StreamStatus         st;
    while ((st = stream.next(e)) == replay::StreamStatus::Ok) {
        w.write(epoch_ns + e.send_ns, from, to, e.bytes);
    }
    const replay::StreamStats& s = stream.stats();
    std::printf("messages %llu\npackets %llu\nheartbeats %llu\nrecords %llu\n",
                static_cast<unsigned long long>(s.messages),
                static_cast<unsigned long long>(s.packets),
                static_cast<unsigned long long>(s.heartbeats),
                static_cast<unsigned long long>(w.records()));
    std::printf(
        "closed full %llu window %llu cap %llu end %llu\n",
        static_cast<unsigned long long>(s.closed[0]), static_cast<unsigned long long>(s.closed[1]),
        static_cast<unsigned long long>(s.closed[2]), static_cast<unsigned long long>(s.closed[3]));
    std::printf("clamped timestamps %llu\ntruncated input %s\n",
                static_cast<unsigned long long>(s.clamped_timestamps),
                s.truncated_input ? "yes" : "no");
    if (st == replay::StreamStatus::Malformed) {
        std::fprintf(stderr, "ttt_replay: input has a malformed frame after %llu messages\n",
                     static_cast<unsigned long long>(s.messages));
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) try {
    std::string          input;
    std::string          out;
    pcap::Endpoint       to;
    pcap::Endpoint       from;
    replay::StreamConfig cfg;
    u64                  max_messages = ~u64{0};
    u64                  epoch_ns = 0;
    std::string          session = "TTTSESSION";

    (void)pcap::parse_endpoint("239.1.1.1:30001", to);
    (void)pcap::parse_endpoint("10.0.0.1:40000", from);
    cfg.packets_per_second = 10'000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              val = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage((a + " needs a value").c_str());
            }
            return argv[++i];
        };
        if (a == "--input") {
            input = val();
        } else if (a == "--pcap") {
            out = val();
        } else if (a == "--group") {
            if (!pcap::parse_endpoint(val(), to)) usage("bad --group");
        } else if (a == "--source") {
            if (!pcap::parse_endpoint(val(), from)) usage("bad --source");
        } else if (a == "--session") {
            session = val();
            if (session.size() > mold::kSessionSize) usage("session is longer than 10 chars");
        } else if (a == "--rate") {
            cfg.pacing = replay::Pacing::FixedRate;
            cfg.packets_per_second = number("--rate", val());
            if (cfg.packets_per_second == 0) usage("--rate must be positive");
        } else if (a == "--feed-time") {
            cfg.pacing = replay::Pacing::FeedTime;
            cfg.speed = number("--feed-time", val());
            if (cfg.speed == 0) usage("--feed-time must be positive");
        } else if (a == "--flush-window-ns") {
            cfg.flush_window_ns = number("--flush-window-ns", val());
        } else if (a == "--heartbeat-ns") {
            cfg.heartbeat_interval_ns = number("--heartbeat-ns", val());
        } else if (a == "--budget") {
            cfg.budget = number("--budget", val());
            if (cfg.budget < 72 || cfg.budget > 65507) usage("--budget must be in 72..65507");
        } else if (a == "--max-per-packet") {
            const u64 n = number("--max-per-packet", val());
            if (n > mold::kMaxMessageCount) usage("--max-per-packet too large");
            cfg.max_messages_per_packet = static_cast<u16>(n);
        } else if (a == "--max-messages") {
            max_messages = number("--max-messages", val());
        } else if (a == "--epoch-ns") {
            epoch_ns = number("--epoch-ns", val());
        } else {
            usage(("unknown option " + a).c_str());
        }
    }
    if (input.empty() || out.empty()) {
        usage("--input and --pcap are required");
    }
    cfg.session = mold::Session(session);

    if (ends_with(input, ".gz")) {
        using Gz = FrameReader<GzipSource<FdSource>>;
        return run(Limited<Gz>(Gz(GzipSource<FdSource>(FdSource(input))), max_messages), cfg, out,
                   from, to, epoch_ns);
    }
    using Plain = FrameReader<FdSource>;
    return run(Limited<Plain>(Plain(FdSource(input)), max_messages), cfg, out, from, to, epoch_ns);
} catch (const std::exception& ex) {
    std::fprintf(stderr, "ttt_replay: %s\n", ex.what());
    return 1;
}

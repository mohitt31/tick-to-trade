#include "ttt/live/feed_server.hpp"

#include <poll.h>

#include <algorithm>
#include <queue>

#include "itch/book/book_types.hpp"
#include "itch/core/endian.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/measure/trailer.hpp"
#include "ttt/mold/packer.hpp"
#include "ttt/replay/servers.hpp"

namespace ttt::live {

namespace {

using Bytes = std::vector<std::byte>;

constexpr feed::BookCapacity kCap{};

struct Held {
    u64   release_ns;
    u64   order;
    int   feed;
    u64   intended_ns;  // absolute; for the trailer
    Bytes bytes;
};

struct LaterHeld {
    bool operator()(const Held& a, const Held& b) const noexcept {
        return a.release_ns != b.release_ns ? a.release_ns > b.release_ns : a.order > b.order;
    }
};

}  // namespace

struct FeedServer::Impl {
    FeedServerConfig                             cfg;
    std::unique_ptr<AnySource>                   src[2];
    std::vector<replay::PacketStream<SourceRef>> streams;
    replay::RewindServer                         rewind;
    replay::SnapshotServer                       snap;
    net::Fd                                      send_fd;
    net::Fd                                      rewind_fd;
    net::Fd                                      listen_fd;
    Rng                                          rng;
    FeedServerStats                              stats{};

    // Lookahead per stream: the next packet and when it is due.
    bool  have[2] = {false, false};
    bool  done[2] = {false, false};
    u64   due[2] = {0, 0};
    Bytes next_bytes[2];
    bool  next_is_data[2] = {false, false};

    std::priority_queue<Held, std::vector<Held>, LaterHeld> held;
    u64                                                     held_order = 0;
    bool                                                    burst_bad[2] = {false, false};
    u64                                                     start = 0;
    u64                                                     index[2] = {0, 0};
    Bytes                                                   out_buf;

    Impl(const FeedServerConfig& c, std::unique_ptr<AnySource> a, std::unique_ptr<AnySource> b)
        : cfg(c),
          src{std::move(a), std::move(b)},
          rewind(c.session, c.rewind_capacity, c.stream[0].budget),
          snap(c.session, c.symbol, kCap),
          rng(c.faults.seed) {
        for (int f = 0; f < 2; ++f) {
            replay::StreamConfig sc = c.stream[f];
            sc.session = c.session;
            streams.emplace_back(SourceRef(*src[f]), sc);
        }
        net::UdpOptions o;
        o.bind = net::Endpoint{c.iface, 0};
        o.iface = c.iface;
        o.sndbuf = 4 << 20;
        send_fd = net::udp_socket(o);

        net::UdpOptions r;
        r.bind = c.rewind;
        r.iface = c.iface;
        rewind_fd = net::udp_socket(r);
        listen_fd = net::tcp_listen(c.snapshot);

        for (int f = 0; f < 2; ++f) {
            pull(f);
        }
    }

    void pull(int f) {
        replay::Emitted e;
        if (streams[static_cast<std::size_t>(f)].next(e) != replay::StreamStatus::Ok) {
            have[f] = false;
            done[f] = true;
            return;
        }
        have[f] = true;
        due[f] = e.send_ns;
        next_is_data[f] = e.kind == mold::Kind::Messages;
        next_bytes[f].assign(e.bytes.begin(), e.bytes.end());
    }

    [[nodiscard]] bool faults_active() const noexcept {
        return cfg.faults_on && !(done[0] && done[1]);
    }

    void publish(const Bytes& pkt) {
        mold::PacketView v;
        if (mold::decode(pkt, v) != mold::DecodeError::None ||
            v.header().kind() != mold::Kind::Messages) {
            return;
        }
        u64 q = v.header().sequence;
        for (auto m : v) {
            rewind.publish(q, m);
            snap.publish(q, m);
            ++q;
            ++stats.messages;
        }
    }

    // intended_ns is absolute. With the trailer on, the actual send time is
    // stamped as late as possible, after the copy and just before the call.
    void send_now(int f, std::span<const std::byte> bytes, u64 intended_ns) {
        std::span<const std::byte> out = bytes;
        if (cfg.trailer) {
            out_buf.resize(bytes.size() + measure::kTrailerSize);
            std::copy(bytes.begin(), bytes.end(), out_buf.begin());
            const std::span<std::byte> tail =
                std::span<std::byte>(out_buf).subspan(bytes.size(), measure::kTrailerSize);
            measure::store_trailer(tail, measure::Trailer{static_cast<itch::u16>(f), index[f]++,
                                                          intended_ns, now_ns()});
            out = out_buf;
        }
        if (net::send_to(send_fd.get(), cfg.feed[f], out) == net::Io::Ok) {
            ++stats.sent[f];
        } else {
            ++stats.send_errors;
        }
    }

    // The faulty network, at the send path.
    void transmit(int f, const Bytes& bytes, u64 t) {
        const u64 intended = start + due[f];
        if (!faults_active()) {
            send_now(f, bytes, intended);
            return;
        }
        const sim::ChannelFaults& ch = cfg.faults.feed[f];
        bool&                     bad = burst_bad[f];
        bad = bad ? !rng.chance_ppm(ch.burst_exit_ppm) : rng.chance_ppm(ch.burst_enter_ppm);
        bool             drop = bad || rng.chance_ppm(ch.drop_ppm);
        mold::PacketView v;
        if (mold::decode(bytes, v) == mold::DecodeError::None &&
            v.header().kind() == mold::Kind::Messages) {
            for (const auto& [from, to] : cfg.faults.joint_outages) {
                drop = drop || (v.header().sequence < to && v.header().end_sequence() > from);
            }
        }
        if (drop) {
            ++stats.dropped[f];
            return;
        }
        const int copies = rng.chance_ppm(ch.dup_ppm) ? 2 : 1;
        if (copies == 2) {
            ++stats.duplicated[f];
        }
        for (int c = 0; c < copies; ++c) {
            const u64 delay = ch.jitter_ns == 0 ? 0 : rng.range(0, ch.jitter_ns);
            if (delay == 0) {
                send_now(f, bytes, intended);
            } else {
                held.push(Held{t + delay, held_order++, f, intended, bytes});
            }
        }
    }

    void serve_rewind() {
        std::array<std::byte, 64> buf{};
        for (;;) {
            std::size_t   got = 0;
            net::Endpoint from;
            if (net::recv_from(rewind_fd.get(), buf, got, &from) != net::Io::Ok) {
                return;
            }
            const bool ok = rewind.serve(
                std::span<const std::byte>(buf.data(), got), [&](std::span<const std::byte> pkt) {
                    if (faults_active() && rng.chance_ppm(cfg.faults.response_drop_ppm)) {
                        return;
                    }
                    (void)net::send_to(rewind_fd.get(), from, pkt);
                });
            ok ? ++stats.rewind_served : ++stats.rewind_unavailable;
        }
    }

    void serve_snapshots() {
        for (;;) {
            net::Fd c = net::tcp_accept(listen_fd.get());
            if (!c.valid()) {
                return;
            }
            Bytes blob;
            snap.build(blob);
            Bytes framed(4 + blob.size());
            itch::store_be<u32>(framed.data(), static_cast<u32>(blob.size()));
            std::copy(blob.begin(), blob.end(), framed.begin() + 4);
            std::span<const std::byte> out(framed);
            if (faults_active() && rng.chance_ppm(cfg.faults.snapshot_cut_ppm)) {
                out = out.first(framed.size() / 2);
                ++stats.snapshots_cut;
            } else {
                ++stats.snapshots_served;
            }
            (void)net::write_all(c.get(), out);
        }
    }

    FeedServerStats run(const std::atomic<bool>& stop) {
        start = now_ns() + cfg.start_delay_ns;
        u32  eos_left = cfg.eos_repeats;
        u64  eos_due = 0;
        bool eos_scheduled = false;
        u64  eos_seq = cfg.stream[0].first_sequence;

        while (!stop.load(std::memory_order_relaxed)) {
            const u64  now = now_ns();
            const u64  t = now > start ? now - start : 0;
            const bool started = now >= start;

            // Stream packets that are due, earliest first across both feeds.
            while (started) {
                int f = -1;
                for (int i = 0; i < 2; ++i) {
                    if (have[i] && due[i] <= t && (f < 0 || due[i] < due[f])) {
                        f = i;
                    }
                }
                if (f < 0) {
                    break;
                }
                stats.max_lag_ns = std::max(stats.max_lag_ns, t - due[f]);
                if (f == 0 && next_is_data[0]) {
                    publish(next_bytes[0]);  // the servers follow feed A
                }
                transmit(f, next_bytes[f], t);
                pull(f);
            }
            while (!held.empty() && held.top().release_ns <= t) {
                send_now(held.top().feed, held.top().bytes, held.top().intended_ns);
                held.pop();
            }

            if (done[0] && done[1] && !eos_scheduled) {
                eos_scheduled = true;
                eos_due = t + cfg.eos_interval_ns;
                eos_seq = rewind.next_sequence();
            }
            if (eos_scheduled && eos_left > 0 && t >= eos_due) {
                mold::Packer p(cfg.session, eos_seq, mold::kMaxPayloadStandardMtu);
                const auto   eos = p.end_of_session();
                for (int f = 0; f < 2; ++f) {
                    send_now(f, eos, start + eos_due);
                }
                --eos_left;
                eos_due = t + cfg.eos_interval_ns;
            }
            if (eos_scheduled && eos_left == 0 && held.empty()) {
                stats.book_digest = itch::book::book_digest(snap.book().book());
                break;
            }

            serve_rewind();
            serve_snapshots();

            // Wait for the next thing that is due, or for a request.
            u64 next = ~u64{0};
            for (int i = 0; i < 2; ++i) {
                if (have[i]) next = std::min(next, due[i]);
            }
            if (!held.empty()) next = std::min(next, held.top().release_ns);
            if (eos_scheduled && eos_left > 0) next = std::min(next, eos_due);
            const u64 now2 = now_ns();
            const u64 t2 = now2 > start ? now2 - start : 0;
            const u64 wait_ns = next > t2 ? next - t2 : 0;
            if (cfg.busy_wait && started) {
                // Measurement pacing: never sleep, only check for requests.
                pollfd fds[2] = {{rewind_fd.get(), POLLIN, 0}, {listen_fd.get(), POLLIN, 0}};
                (void)::poll(fds, 2, 0);
            } else if (wait_ns > 2'000'000 || !started) {
                pollfd    fds[2] = {{rewind_fd.get(), POLLIN, 0}, {listen_fd.get(), POLLIN, 0}};
                const int ms = static_cast<int>(std::min<u64>(wait_ns / 1'000'000 - 1, 20));
                (void)::poll(fds, 2, std::max(ms, 0));
            } else if (wait_ns > 0) {
                cpu_relax();
            }
        }
        return stats;
    }
};

FeedServer::FeedServer(const FeedServerConfig& cfg, std::unique_ptr<AnySource> a,
                       std::unique_ptr<AnySource> b)
    : impl_(std::make_unique<Impl>(cfg, std::move(a), std::move(b))) {}

FeedServer::~FeedServer() = default;

net::Endpoint FeedServer::rewind_endpoint() const {
    return net::local_endpoint(impl_->rewind_fd.get());
}

net::Endpoint FeedServer::snapshot_endpoint() const {
    return net::local_endpoint(impl_->listen_fd.get());
}

FeedServerStats FeedServer::run(const std::atomic<bool>& stop) { return impl_->run(stop); }

}  // namespace ttt::live

#include "ttt/live/receiver.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/core/endian.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/measure/trailer.hpp"
#include "ttt/mold/mold.hpp"

namespace ttt::live {

namespace {

using Bytes = std::vector<std::byte>;

// A snapshot over TCP: a 4-byte big-endian length, then the snapshot.
class SnapshotClient {
public:
    enum class Step { Idle, Connecting, Reading };

    void start(const net::Endpoint& server) {
        close();
        fd_ = net::tcp_connect_start(server);
        step_ = Step::Connecting;
        buf_.clear();
        ++connects_;
    }

    void close() {
        fd_.reset();
        step_ = Step::Idle;
    }

    [[nodiscard]] int   fd() const noexcept { return fd_.get(); }
    [[nodiscard]] Step  step() const noexcept { return step_; }
    [[nodiscard]] short events() const noexcept {
        return step_ == Step::Connecting ? POLLOUT : POLLIN;
    }
    [[nodiscard]] u64 connects() const noexcept { return connects_; }
    [[nodiscard]] u64 failures() const noexcept { return failures_; }

    // Advances on readiness. Returns true when a complete snapshot is in out.
    bool on_ready(Bytes& out) {
        if (step_ == Step::Connecting) {
            if (net::connect_result(fd_.get()) != 0) {
                fail();
                return false;
            }
            step_ = Step::Reading;
        }
        std::array<std::byte, 1 << 16> chunk{};
        for (;;) {
            std::size_t   got = 0;
            const net::Io io = net::read_some(fd_.get(), chunk, got);
            if (io == net::Io::WouldBlock) {
                return false;
            }
            if (io == net::Io::Error) {
                fail();
                return false;
            }
            if (got == 0) {
                // The server closed. A complete snapshot is used; a cut one is
                // still handed over, because the handler's decoder is what
                // decides, and it rejects a truncated snapshot whole.
                const bool whole =
                    buf_.size() >= 4 && buf_.size() == 4 + itch::load_be<itch::u32>(buf_.data());
                if (buf_.size() < 4) {
                    fail();
                    return false;
                }
                out.assign(buf_.begin() + 4, buf_.end());
                if (!whole) {
                    ++failures_;
                }
                close();
                return true;
            }
            buf_.insert(buf_.end(), chunk.begin(), chunk.begin() + static_cast<long>(got));
        }
    }

private:
    void fail() {
        ++failures_;
        close();
    }

    net::Fd fd_;
    Step    step_ = Step::Idle;
    Bytes   buf_;
    u64     connects_ = 0;
    u64     failures_ = 0;
};

}  // namespace

struct Receiver::Impl {
    struct Actions {
        Impl* r;
        void  apply(u64 seq, std::span<const std::byte> m) {
            r->book.apply(m);
            r->applied = seq;
        }
        void begin_snapshot(u64) { r->book.reset(); }
        void apply_snapshot(std::span<const std::byte> m) { r->book.apply(m); }
        void end_snapshot(u64 seq) { r->applied = seq; }
        void request_retransmit(u64 seq, itch::u16 count) {
            const auto req = mold::encode_request(r->cfg.line.session, seq, count);
            (void)net::send_to(r->rewind_fd.get(), r->cfg.rewind_server, req);
        }
        void request_snapshot() { r->snap.start(r->cfg.snapshot_server); }
    };

    ReceiverConfig             cfg;
    net::Fd                    feed_fd[2];
    net::Fd                    rewind_fd;
    SnapshotClient             snap;
    feed::BookSink             book;
    u64                        applied = 0;
    Actions                    actions{this};
    feed::LineHandler<Actions> handler;

    explicit Impl(const ReceiverConfig& c)
        : cfg(c), book(c.symbol, c.book), handler(c.line, actions) {
        for (int f = 0; f < 2; ++f) {
            net::UdpOptions o;
            o.bind = c.feed[f];
            o.iface = c.iface;
            o.join = c.feed[f].multicast();
            o.rcvbuf = c.rcvbuf;
            feed_fd[f] = net::udp_socket(o);
        }
        net::UdpOptions r;
        r.bind = net::Endpoint{c.iface, 0};
        r.iface = c.iface;
        rewind_fd = net::udp_socket(r);
    }

    ReceiverResult run(const std::atomic<bool>& stop) {
        ReceiverResult res;
        if (cfg.record_latency) {
            res.latency.emplace();
        }
        std::array<std::byte, 65536> buf{};
        u64                          last_rx = now_ns();
        Bytes                        snapshot;

        while (!stop.load(std::memory_order_relaxed)) {
            if (handler.ended() && !handler.stale()) {
                res.stopped_because = "end of session, book complete";
                break;
            }
            const u64 now = now_ns();
            if (now - last_rx > cfg.idle_timeout_ns) {
                res.stopped_because = "idle timeout";
                break;
            }

            const u64 deadline = handler.next_deadline();
            int       timeout_ms = 10;
            if (deadline != feed::kNever) {
                timeout_ms = deadline <= now ? 0
                                             : static_cast<int>(std::min<u64>(
                                                   (deadline - now + 999'999) / 1'000'000, 10));
            }

            std::array<pollfd, 4> fds{};
            nfds_t                n = 0;
            fds[n++] = {feed_fd[0].get(), POLLIN, 0};
            fds[n++] = {feed_fd[1].get(), POLLIN, 0};
            fds[n++] = {rewind_fd.get(), POLLIN, 0};
            if (snap.step() != SnapshotClient::Step::Idle) {
                fds[n++] = {snap.fd(), snap.events(), 0};
            }
            (void)::poll(fds.data(), n, timeout_ms);

            const Source sources[3] = {Source::A, Source::B, Source::Rewind};
            const int    fdsrc[3] = {feed_fd[0].get(), feed_fd[1].get(), rewind_fd.get()};
            for (int i = 0; i < 3; ++i) {
                if ((fds[static_cast<std::size_t>(i)].revents & POLLIN) == 0) {
                    continue;
                }
                for (;;) {
                    std::size_t got = 0;
                    if (net::recv_from(fdsrc[i], buf, got) != net::Io::Ok) {
                        break;
                    }
                    const u64                  t_recv = now_ns();
                    std::span<const std::byte> payload(buf.data(), got);
                    last_rx = t_recv;
                    // Only the feeds carry trailers; retransmissions do not.
                    const bool timed = cfg.trailer && sources[i] != Source::Rewind;
                    if (timed) {
                        const auto t = measure::split_trailer(payload);
                        if (!t) {
                            if (res.latency) ++res.latency->without_trailer;
                        } else if (res.latency) {
                            res.latency->intended_to_recv.record(
                                signed_diff(t_recv, t->intended_ns));
                            res.latency->sent_to_recv.record(signed_diff(t_recv, t->sent_ns));
                            res.latency->sender_lag.record(signed_diff(t->sent_ns, t->intended_ns));
                        }
                    }
                    handler.on_packet(sources[i], payload, t_recv);
                    if (timed && res.latency) {
                        res.latency->handler.record(signed_diff(now_ns(), t_recv));
                    }
                }
            }
            if (n == 4 && fds[3].revents != 0) {
                if (snap.on_ready(snapshot)) {
                    last_rx = now_ns();
                    handler.on_snapshot(snapshot, last_rx);
                }
            }
            handler.on_timer(now_ns());
        }
        if (res.stopped_because.empty()) {
            res.stopped_because = "stopped";
        }
        res.line = handler.stats();
        res.applied = applied;
        res.digest = itch::book::book_digest(book.book());
        res.ended = handler.ended();
        res.stale = handler.stale();
        res.snapshot_connects = snap.connects();
        res.snapshot_failures = snap.failures();
        return res;
    }

    using Source = feed::Source;

    static itch::i64 signed_diff(u64 a, u64 b) noexcept {
        return a >= b ? static_cast<itch::i64>(a - b) : -static_cast<itch::i64>(b - a);
    }
};

Receiver::Receiver(const ReceiverConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
Receiver::~Receiver() = default;

net::Endpoint Receiver::feed_endpoint(int f) const {
    return net::local_endpoint(impl_->feed_fd[f].get());
}

void Receiver::set_servers(const net::Endpoint& rewind, const net::Endpoint& snapshot) {
    impl_->cfg.rewind_server = rewind;
    impl_->cfg.snapshot_server = snapshot;
}

ReceiverResult Receiver::run(const std::atomic<bool>& stop) { return impl_->run(stop); }

}  // namespace ttt::live

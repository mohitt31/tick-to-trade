// The measured receiver: one feed, one receive path, the handler and the book.
//
// The same loop for every path, so a latency difference between two runs is a
// difference between paths and nothing else. Per datagram: split off the
// measurement trailer, record schedule-to-receive and send-to-receive against
// the path's receive timestamp, hand the packet to the LineHandler, which
// applies it to the book, and record how long that took.
//
// Recovery is not part of a measurement run. Loss is counted from sequence
// numbers alone, by the same audit that checks pcaps, after the timed part of
// each datagram: `missing` is messages that never arrived. That is different
// from what the handler could not apply, since one unfilled gap holds back
// everything behind it. A run with any missing message is a run whose latency
// numbers are suspect, and the report says so.
#pragma once

#include <atomic>
#include <cstddef>
#include <span>
#include <string>

#include "itch/book/book_types.hpp"
#include "itch/core/types.hpp"
#include "ttt/audit/sequence_audit.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/line_handler.hpp"
#include "ttt/live/receiver.hpp"
#include "ttt/measure/histogram.hpp"
#include "ttt/measure/trailer.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/net/socket.hpp"
#include "ttt/rx/socket_paths.hpp"

namespace ttt::rx {

using itch::u64;

struct BenchConfig {
    feed::LineConfig   line{};
    std::string        symbol = "SYM1";
    feed::BookCapacity book{};
    bool               trailer = true;
    u64                idle_timeout_ns = 5'000'000'000;
    net::Endpoint      rewind_server{};  // port 0: no retransmission requests
    u64         warmup_ns = 0;  // after the first datagram, latency is not recorded for this long
    // Datagrams whose schedule-to-handled time exceeds this are logged with
    // their receive time and sequence number, for correlating with kernel
    // events. 0 disables the log. The log is allocated up front and never grows
    // during a run; outliers past its capacity are counted, not stored.
    u64         outlier_threshold_ns = 0;
    std::size_t outlier_capacity = 1'000'000;
};

struct BenchResult {
    live::ReceiverResult rx;
    measure::Histogram   batch{1'000'000};  // datagrams per receive call that returned data
    PathStats            path{};
    audit::FlowReport    sequence{};     // what arrived, by sequence number
    u64                  missing = 0;    // messages that never arrived
    u64                  unapplied = 0;  // arrived, but held behind a gap

    struct Outlier {
        u64       recv_ns;           // monotonic, when the receive call returned
        u64       sequence;          // first message in the packet
        itch::i64 intended_to_recv;  // schedule to receive
        itch::i64 sent_to_recv;      // send call to receive
        itch::i64 handled;           // schedule to the handler returning
    };
    std::vector<Outlier> outliers;
    u64                  outliers_dropped = 0;
};

template <class Path>
BenchResult run_bench(Path& path, const BenchConfig& cfg, const std::atomic<bool>& stop) {
    struct Actions {
        feed::BookSink* book;
        u64*            applied;
        int             rewind_fd;
        net::Endpoint   rewind;
        mold::Session   session;

        void apply(u64 seq, std::span<const std::byte> m) {
            book->apply(m);
            *applied = seq;
        }
        void begin_snapshot(u64) { book->reset(); }
        void apply_snapshot(std::span<const std::byte> m) { book->apply(m); }
        void end_snapshot(u64 seq) { *applied = seq; }
        void request_retransmit(u64 seq, itch::u16 count) {
            if (rewind.port != 0) {
                (void)net::send_to(rewind_fd, rewind, mold::encode_request(session, seq, count));
            }
        }
        void request_snapshot() {}
    };

    BenchResult res;
    res.rx.latency.emplace();
    auto& lat = *res.rx.latency;
    if (cfg.outlier_threshold_ns != 0) {
        res.outliers.reserve(cfg.outlier_capacity);
    }

    feed::BookSink book(cfg.symbol, cfg.book);
    u64            applied = 0;
    net::Fd        rewind_fd;
    if (cfg.rewind_server.port != 0) {
        net::UdpOptions o;
        o.bind = net::Endpoint{net::kAny, 0};
        rewind_fd = net::udp_socket(o);
    }
    Actions actions{&book, &applied, rewind_fd.get(), cfg.rewind_server, cfg.line.session};
    feed::LineHandler<Actions> handler(cfg.line, actions);

    auto diff = [](u64 a, u64 b) -> itch::i64 {
        return a >= b ? static_cast<itch::i64>(a - b) : -static_cast<itch::i64>(b - a);
    };

    audit::SequenceAudit seq;
    const net::Endpoint  flow{};
    u64                  last_rx = now_ns();
    u64                  record_from = 0;  // set by the first datagram
    for (;;) {
        if (stop.load(std::memory_order_relaxed)) {
            res.rx.stopped_because = "stopped";
            break;
        }
        if (handler.ended() && !handler.stale()) {
            res.rx.stopped_because = "end of session, book complete";
            break;
        }
        const std::size_t n = path.receive([&](std::span<const std::byte> dgram, u64 t_recv) {
            if (record_from == 0) {
                record_from = t_recv + cfg.warmup_ns;
            }
            const bool recording = t_recv >= record_from;
            u64        intended = 0;
            u64        sent = 0;
            if (cfg.trailer) {
                // The trailer is stripped from every datagram, warmup or not;
                // only whether its times are recorded depends on the warmup.
                const auto t = measure::split_trailer(dgram);
                if (!recording) {
                } else if (t) {
                    lat.intended_to_recv.record(diff(t_recv, t->intended_ns));
                    lat.sent_to_recv.record(diff(t_recv, t->sent_ns));
                    lat.sender_lag.record(diff(t->sent_ns, t->intended_ns));
                    intended = t->intended_ns;
                    sent = t->sent_ns;
                } else {
                    ++lat.without_trailer;
                }
            }
            handler.on_packet(feed::Source::A, dgram, t_recv);
            if (recording) {
                const u64 done = now_ns();
                lat.handler.record(diff(done, t_recv));
                if (cfg.outlier_threshold_ns != 0 && intended != 0 &&
                    done - intended > cfg.outlier_threshold_ns) {
                    if (res.outliers.size() < res.outliers.capacity()) {
                        const u64 first =
                            dgram.size() >= mold::kHeaderSize
                                ? itch::load_be<u64>(dgram.data() + mold::kSessionSize)
                                : 0;
                        res.outliers.push_back({t_recv, first, diff(t_recv, intended),
                                                diff(t_recv, sent), diff(done, intended)});
                    } else {
                        ++res.outliers_dropped;
                    }
                }
            }
            seq.observe(t_recv, flow, dgram);  // after the timed part
        });
        const u64         now = now_ns();
        if (n != 0) {
            res.batch.record(static_cast<itch::i64>(n));
            last_rx = now;
        } else if (now - last_rx > cfg.idle_timeout_ns) {
            // Nothing for this long after end of session means the tail is
            // missing; before it, that the feed never started or stopped.
            res.rx.stopped_because =
                handler.ended() ? "idle after end of session with a gap" : "idle timeout";
            break;
        }
        handler.on_timer(now);
    }

    res.rx.line = handler.stats();
    res.rx.applied = applied;
    res.rx.digest = itch::book::book_digest(book.book());
    res.rx.ended = handler.ended();
    res.rx.stale = handler.stale();
    res.path = path.stats();
    res.unapplied = handler.frontier() - handler.next_expected();
    if (!seq.flows().empty()) {
        res.sequence = seq.flows().begin()->second;
        res.missing = res.sequence.missing_messages();
    }
    return res;
}

}  // namespace ttt::rx

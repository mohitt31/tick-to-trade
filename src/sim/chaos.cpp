#include "ttt/sim/chaos.hpp"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "itch/book/book_types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/replay/packet_stream.hpp"
#include "ttt/replay/servers.hpp"

namespace ttt::sim {

namespace {

using Bytes = std::vector<std::byte>;
using feed::Source;

// Small books: a run builds several and the harness runs many thousands of
// seeds. The sanitizer builds hold 4096 freed slots back from reuse, so the
// pools need that much headroom above the generator's live orders.
constexpr feed::BookCapacity kCap{16'384, 8'192, 4'096};

const mold::Session kSession{"CHAOS"};

enum class Kind : itch::u8 {
    SenderNext,  // src: which stream
    Deliver,     // src: A, B or Rewind
    RewindRequest,
    SnapshotBuild,
    SnapshotDeliver,
    Timer,
    EndOfSession,  // src: which feed
};

struct Event {
    u64    t = 0;
    u64    order = 0;  // tie-break, so equal times run in scheduling order
    Kind   kind = Kind::Deliver;
    Source src = Source::A;
    Bytes  bytes;
};

struct Later {
    bool operator()(const Event& a, const Event& b) const noexcept {
        return a.t != b.t ? a.t > b.t : a.order > b.order;
    }
};

bool same_book(const feed::BookSink& x, const feed::BookSink& y) {
    const auto& a = x.book();
    const auto& b = y.book();
    if (!(a.stats() == b.stats())) {
        return false;
    }
    for (itch::Side s : {itch::Side::Buy, itch::Side::Sell}) {
        if (a.empty(s) != b.empty(s) || (!a.empty(s) && a.best(s) != b.best(s))) {
            return false;
        }
    }
    return true;
}

class Harness {
public:
    explicit Harness(const ChaosConfig& cfg)
        : cfg_(cfg),
          rng_(cfg.seed ^ 0xC4A05C4A05ULL),
          msgs_([&] {
              Rng g(cfg.seed);
              return generate(g, cfg.gen);
          }()),
          input_(frame(msgs_)),
          rewind_(kSession, cfg.rewind_capacity, cfg.budget_a),
          snap_(kSession, cfg.symbol, kCap),
          oracle_(cfg.symbol, kCap),
          book_(cfg.symbol, kCap),
          actions_{this},
          handler_(line_config(cfg), actions_) {
        for (int f = 0; f < 2; ++f) {
            replay::StreamConfig sc;
            sc.session = kSession;
            sc.pacing = replay::Pacing::FeedTime;
            sc.budget = f == 0 ? cfg.budget_a : cfg.budget_b;
            sc.flush_window_ns = cfg.flush_window_ns;
            sc.heartbeat_interval_ns = cfg.heartbeat_ns;
            streams_.emplace_back(itch::wire::FrameCursor(input_), sc);
        }
        for (u64 i = 0; i < 2; ++i) {
            pull(static_cast<Source>(i));
        }
    }

    ChaosResult run() {
        while (!events_.empty() && result_.ok) {
            // priority_queue::top is const; the payload is moved out before pop.
            Event e = std::move(const_cast<Event&>(events_.top()));
            events_.pop();
            now_ = e.t;
            ++result_.events;
            if (streams_done_ == 2 && now_ > faults_end_ + cfg_.quiet_limit_ns) {
                fail("did not converge within the quiet limit");
                break;
            }
            dispatch(e);
        }
        if (result_.ok) {
            finish();
        }
        result_.messages = msgs_.size();
        result_.end_ns = now_;
        result_.line = handler_.stats();
        result_.retransmits_served = rewind_.served();
        result_.retransmits_unavailable = rewind_.unavailable();
        return result_;
    }

private:
    // --- what the line handler asks for ----------------------------------

    struct Actions {
        Harness* h;

        void apply(u64 seq, std::span<const std::byte> msg) { h->on_apply(seq, msg); }
        void begin_snapshot(u64 seq) { h->on_begin_snapshot(seq); }
        void apply_snapshot(std::span<const std::byte> msg) { h->book_.apply(msg); }
        void end_snapshot(u64 seq) { h->on_end_snapshot(seq); }
        void request_retransmit(u64 seq, u16 count) { h->on_request_retransmit(seq, count); }
        void request_snapshot() { h->on_request_snapshot(); }
    };

    static feed::LineConfig line_config(const ChaosConfig& cfg) {
        feed::LineConfig lc = cfg.line;
        lc.session = kSession;
        return lc;
    }

    static Bytes frame(const std::vector<itch_out::Message>& msgs) {
        Bytes out;
        for (const auto& m : msgs) {
            const auto b = m.bytes();
            const auto at = out.size();
            out.resize(at + 2 + b.size());
            itch::store_be<u16>(out.data() + at, static_cast<u16>(b.size()));
            std::copy(b.begin(), b.end(), out.begin() + static_cast<std::ptrdiff_t>(at + 2));
        }
        return out;
    }

    void on_apply(u64 seq, std::span<const std::byte> msg) {
        if (seq != applied_ + 1) {
            fail("applied sequence " + std::to_string(seq) + " after " + std::to_string(applied_));
            return;
        }
        if (seq > msgs_.size()) {
            fail("applied a sequence number that was never sent");
            return;
        }
        book_.apply(msg);
        applied_ = seq;
        advance_oracle(seq);
        ++result_.message_checks;
        if (!same_book(book_, oracle_)) {
            fail("book differs from the oracle after message " + std::to_string(seq));
        }
    }

    void on_begin_snapshot(u64 seq) {
        if (seq < applied_) {
            fail("snapshot at " + std::to_string(seq) + " would move the book back from " +
                 std::to_string(applied_));
            return;
        }
        book_.reset();
    }

    void on_end_snapshot(u64 seq) {
        applied_ = seq;
        advance_oracle(seq);
        full_check("after a snapshot at " + std::to_string(seq));
    }

    void on_request_retransmit(u64 seq, u16 count) {
        if (faulty() && rng_.chance_ppm(cfg_.request_drop_ppm)) {
            return;
        }
        const auto req = mold::encode_request(kSession, seq, count);
        schedule(now_ + cfg_.rewind_delay_ns, Kind::RewindRequest, Source::Rewind,
                 Bytes(req.begin(), req.end()));
    }

    void on_request_snapshot() {
        if (faulty() && rng_.chance_ppm(cfg_.snapshot_fail_ppm)) {
            return;  // the connection failed; the receiver's timeout has to notice
        }
        schedule(now_ + cfg_.snapshot_delay_ns / 2, Kind::SnapshotBuild, Source::Rewind, {});
    }

    // --- the event loop ---------------------------------------------------

    void dispatch(Event& e) {
        switch (e.kind) {
            case Kind::SenderNext: send_from_stream(e); break;
            case Kind::Deliver:
                handler_.on_packet(e.src, e.bytes, now_);
                after_step();
                break;
            case Kind::RewindRequest:
                rewind_.serve(e.bytes, [&](std::span<const std::byte> pkt) {
                    if (faulty() && rng_.chance_ppm(cfg_.response_drop_ppm)) {
                        return;
                    }
                    schedule(now_ + cfg_.rewind_delay_ns, Kind::Deliver, Source::Rewind,
                             Bytes(pkt.begin(), pkt.end()));
                });
                break;
            case Kind::SnapshotBuild: {
                Bytes snap;
                snap_.build(snap);
                ++result_.snapshots_built;
                schedule(now_ + cfg_.snapshot_delay_ns / 2, Kind::SnapshotDeliver, Source::Rewind,
                         std::move(snap));
                break;
            }
            case Kind::SnapshotDeliver:
                handler_.on_snapshot(e.bytes, now_);
                after_step();
                break;
            case Kind::Timer:
                // This timer has fired, so an equal deadline set later needs a
                // new event rather than being taken for this one.
                if (e.t == timer_scheduled_) {
                    timer_scheduled_ = feed::kNever;
                }
                if (e.t == handler_.next_deadline()) {
                    handler_.on_timer(now_);
                    after_step();
                }
                break;
            case Kind::EndOfSession: {
                mold::Packer p(kSession, msgs_.size() + 1, mold::kMaxPayloadStandardMtu);
                const auto   eos = p.end_of_session();
                transmit(e.src, eos);
                if (--eos_left_[static_cast<int>(e.src)] > 0) {
                    schedule(now_ + cfg_.heartbeat_ns, Kind::EndOfSession, e.src, {});
                }
                break;
            }
        }
    }

    void send_from_stream(Event& e) {
        if (e.src == Source::A) {
            // The servers see what the sender publishes, when feed A publishes
            // it, whatever the network then does to the packet.
            mold::PacketView v;
            if (mold::decode(e.bytes, v) == mold::DecodeError::None &&
                v.header().kind() == mold::Kind::Messages) {
                u64 q = v.header().sequence;
                for (auto m : v) {
                    rewind_.publish(q, m);
                    snap_.publish(q, m);
                    ++q;
                }
            }
        }
        transmit(e.src, e.bytes);
        pull(e.src);
    }

    // Takes the next packet from a stream and schedules it at its send time.
    void pull(Source src) {
        auto&           s = streams_[static_cast<std::size_t>(src)];
        replay::Emitted out;
        const auto      st = s.next(out);
        if (st != replay::StreamStatus::Ok) {
            ++streams_done_;
            if (streams_done_ == 2) {
                faults_end_ = now_;
                for (u64 i = 0; i < 2; ++i) {
                    eos_left_[i] = cfg_.eos_repeats;
                    schedule(now_ + cfg_.heartbeat_ns, Kind::EndOfSession, static_cast<Source>(i),
                             {});
                }
            }
            return;
        }
        schedule(out.send_ns, Kind::SenderNext, src, Bytes(out.bytes.begin(), out.bytes.end()));
    }

    [[nodiscard]] bool faulty() const noexcept { return streams_done_ < 2; }

    // One feed's network: loss, bursts, joint outages, duplication, delay.
    void transmit(Source src, std::span<const std::byte> pkt) {
        const auto& ch = cfg_.feed[static_cast<int>(src)];
        const Bytes bytes(pkt.begin(), pkt.end());
        if (!faulty()) {
            schedule(now_ + ch.base_delay_ns, Kind::Deliver, src, bytes);
            return;
        }
        bool& bad = burst_bad_[static_cast<int>(src)];
        bad = bad ? !rng_.chance_ppm(ch.burst_exit_ppm) : rng_.chance_ppm(ch.burst_enter_ppm);
        if (bad || rng_.chance_ppm(ch.drop_ppm)) {
            return;
        }
        for (const auto& [from, to] : cfg_.joint_outages) {
            if (now_ >= from && now_ < to) {
                return;
            }
        }
        const int copies = rng_.chance_ppm(ch.dup_ppm) ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            const u64 jitter = ch.jitter_ns == 0 ? 0 : rng_.range(0, ch.jitter_ns);
            schedule(now_ + ch.base_delay_ns + jitter, Kind::Deliver, src, bytes);
        }
    }

    void schedule(u64 t, Kind kind, Source src, Bytes bytes) {
        events_.push(Event{t, order_++, kind, src, std::move(bytes)});
    }

    void after_step() {
        if (!result_.ok) {
            return;
        }
        full_check("after a step");
        const u64 d = handler_.next_deadline();
        if (d != feed::kNever && d != timer_scheduled_) {
            timer_scheduled_ = d;
            schedule(std::max(d, now_), Kind::Timer, Source::A, {});
        }
    }

    void advance_oracle(u64 seq) {
        while (oracle_seq_ < seq && oracle_seq_ < msgs_.size()) {
            oracle_.apply(msgs_[oracle_seq_].bytes());
            ++oracle_seq_;
        }
    }

    void full_check(const std::string& where) {
        if (!result_.ok) {
            return;
        }
        ++result_.digest_checks;
        if (itch::book::book_digest(book_.book()) != itch::book::book_digest(oracle_.book())) {
            fail("book digest differs from the oracle at sequence " + std::to_string(applied_) +
                 " " + where);
        }
    }

    void finish() {
        const u64 total = msgs_.size();
        if (handler_.next_expected() != total + 1 || applied_ != total) {
            fail("did not converge: applied " + std::to_string(applied_) + " of " +
                 std::to_string(total) + ", state " +
                 std::to_string(static_cast<int>(handler_.state())));
            return;
        }
        if (handler_.stale()) {
            fail("still stale after the stream ended and every gap closed");
            return;
        }
        if (!handler_.ended()) {
            fail("never saw end of session");
            return;
        }
        if (book_.malformed() != 0) {
            fail("the receiver book was handed a malformed message");
            return;
        }
        full_check("at the end");
    }

    void fail(std::string why) {
        if (result_.ok) {
            result_.ok = false;
            result_.failure = std::move(why);
        }
    }

    ChaosConfig                                                cfg_;
    Rng                                                        rng_;
    std::vector<itch_out::Message>                             msgs_;
    Bytes                                                      input_;
    std::vector<replay::PacketStream<itch::wire::FrameCursor>> streams_;
    replay::RewindServer                                       rewind_;
    replay::SnapshotServer                                     snap_;
    feed::BookSink                                             oracle_;
    u64                                                        oracle_seq_ = 0;
    feed::BookSink                                             book_;
    u64                                                        applied_ = 0;
    Actions                                                    actions_;
    feed::LineHandler<Actions>                                 handler_;

    std::priority_queue<Event, std::vector<Event>, Later> events_;
    u64                                                   order_ = 0;
    u64                                                   now_ = 0;
    int                                                   streams_done_ = 0;
    u64                                                   faults_end_ = 0;
    u64                                                   timer_scheduled_ = feed::kNever;
    bool                                                  burst_bad_[2] = {false, false};
    u32                                                   eos_left_[2] = {0, 0};
    ChaosResult                                           result_{};
};

}  // namespace

ChaosConfig random_config(u64 seed) {
    Rng         r(seed * 0x9E3779B97F4A7C15ULL + 1);
    ChaosConfig c;
    c.seed = seed;

    c.gen.symbols = static_cast<u32>(r.range(1, 4));
    c.gen.messages = static_cast<u32>(r.range(50, 3000));
    c.gen.max_live = static_cast<u32>(r.range(3, 300));
    c.gen.max_gap_ns = r.range(0, 50'000);
    c.gen.silence_ppm = static_cast<u32>(r.range(0, 5'000));
    c.gen.silence_ns = r.range(1, 40) * 1'000'000;

    c.budget_a = r.range(72, mold::kMaxPayloadStandardMtu);
    c.budget_b = r.chance_ppm(300'000) ? c.budget_a : r.range(72, mold::kMaxPayloadStandardMtu);
    c.flush_window_ns = r.range(0, 4) * 50'000;
    c.heartbeat_ns = r.range(1, 10) * 10'000'000;

    // A rough length of the stream, to place outages inside it.
    const u64 approx_ns =
        c.gen.messages * (c.gen.max_gap_ns / 2) +
        (c.gen.messages * static_cast<u64>(c.gen.silence_ppm) / 1'000'000) * c.gen.silence_ns + 1;

    for (int f = 0; f < 2; ++f) {
        ChannelFaults& ch = c.feed[f];
        switch (r.range(0, 5)) {
            case 0: break;                                                            // clean
            case 1: ch.drop_ppm = static_cast<u32>(r.range(1, 20'000)); break;        // light loss
            case 2: ch.drop_ppm = static_cast<u32>(r.range(50'000, 400'000)); break;  // heavy
            case 3:
                ch.burst_enter_ppm = static_cast<u32>(r.range(1'000, 50'000));
                ch.burst_exit_ppm = static_cast<u32>(r.range(50'000, 500'000));
                break;
            case 4: ch.drop_ppm = 1'000'000; break;  // this feed is dead
            default:
                ch.drop_ppm = static_cast<u32>(r.range(0, 50'000));
                ch.burst_enter_ppm = static_cast<u32>(r.range(0, 20'000));
                ch.burst_exit_ppm = static_cast<u32>(r.range(100'000, 900'000));
                break;
        }
        ch.dup_ppm = r.chance_ppm(400'000) ? static_cast<u32>(r.range(1, 200'000)) : 0;
        ch.base_delay_ns = r.range(1, 100) * 1'000;
        ch.jitter_ns = r.chance_ppm(400'000) ? r.range(1, 2'000) * 1'000 : 0;
    }

    const u64 outages = r.chance_ppm(400'000) ? r.range(1, 3) : 0;
    for (u64 i = 0; i < outages; ++i) {
        const u64 from = r.range(0, approx_ns);
        c.joint_outages.emplace_back(from, from + r.range(1, 20'000) * 1'000);
    }
    if (r.chance_ppm(150'000)) {
        // Both feeds dark over the end of the stream: only the end of session
        // repeats reveal that the tail is missing.
        const u64 from = approx_ns > 5'000'000 ? approx_ns - r.range(0, 5'000'000) : 0;
        c.joint_outages.emplace_back(from, ~u64{0});
    }

    c.rewind_capacity =
        r.chance_ppm(200'000) ? static_cast<u32>(1u << r.range(3, 8)) : static_cast<u32>(1u << 16);
    c.request_drop_ppm = static_cast<u32>(r.chance_ppm(300'000) ? r.range(0, 500'000) : 0);
    c.response_drop_ppm = static_cast<u32>(
        r.chance_ppm(100'000) ? 1'000'000 : (r.chance_ppm(300'000) ? r.range(0, 500'000) : 0));
    c.rewind_delay_ns = r.range(1, 500) * 1'000;
    c.snapshot_fail_ppm = static_cast<u32>(r.chance_ppm(300'000) ? r.range(0, 800'000) : 0);
    c.snapshot_delay_ns = r.range(1, 20) * 1'000'000;

    c.line.arb_wait_ns = r.range(0, 1'000) * 1'000;
    c.line.retransmit_timeout_ns = r.range(1, 10) * 1'000'000;
    c.line.max_retransmit_attempts = static_cast<u32>(r.range(1, 4));
    c.line.max_retransmit_count = r.chance_ppm(250'000) ? r.range(1, 20) : r.range(100, 2'000);
    c.line.snapshot_timeout_ns = r.range(20, 200) * 1'000'000;
    c.line.buffer_capacity =
        r.chance_ppm(250'000) ? static_cast<u32>(1u << r.range(3, 6)) : static_cast<u32>(1u << 14);
    return c;
}

ChaosResult run(const ChaosConfig& cfg) { return Harness(cfg).run(); }

}  // namespace ttt::sim

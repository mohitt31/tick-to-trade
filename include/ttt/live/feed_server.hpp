// The sender, on real sockets: feed A and feed B, a rewind server on UDP and a
// snapshot server on TCP, in one thread.
//
// It is the chaos harness's sender side with the simulated network replaced by
// sockets. The same PacketStream decides what goes in each packet and when, and
// the same RewindServer and SnapshotServer answer requests. Faults can be
// switched on at the send path, so the receiver's recovery runs against real
// sockets too, not only in simulation.
//
// Pacing here is good enough for correctness runs on any machine. The pacing
// used for latency measurement on Linux gets its own pinned sender thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "itch/core/types.hpp"
#include "itch/wire/framing.hpp"
#include "ttt/net/endpoint.hpp"
#include "ttt/net/socket.hpp"
#include "ttt/replay/packet_stream.hpp"
#include "ttt/sim/chaos.hpp"

namespace ttt::live {

using itch::u32;
using itch::u64;

// A message source behind a virtual call, so the server is not a template over
// every kind of input. One virtual call per message is nothing next to a send.
class AnySource {
public:
    virtual ~AnySource() = default;
    virtual itch::wire::FrameStatus next(std::span<const std::byte>& out) = 0;
};

class SourceRef {
public:
    explicit SourceRef(AnySource& s) noexcept : s_(&s) {}
    itch::wire::FrameStatus next(std::span<const std::byte>& out) { return s_->next(out); }

private:
    AnySource* s_;
};

// Adapts anything with the MessageSource shape.
template <replay::MessageSource S>
class OwnedSource final : public AnySource {
public:
    explicit OwnedSource(S s) : s_(std::move(s)) {}
    itch::wire::FrameStatus next(std::span<const std::byte>& out) override { return s_.next(out); }

private:
    S s_;
};

struct SendFaults {
    u64                              seed = 1;
    sim::ChannelFaults               feed[2]{};
    // Message sequence ranges [from, to) lost on both feeds: any data packet
    // carrying one of them is dropped. By sequence rather than by time, because
    // on a real clock a sender that stalls for a moment would send the packets
    // due inside a time window late, and they would escape it.
    std::vector<std::pair<u64, u64>> joint_outages;
    u32                              response_drop_ppm = 0;
    u32                              snapshot_cut_ppm = 0;  // send half a snapshot, then close
};

struct FeedServerConfig {
    mold::Session        session{"TTTLIVE"};
    net::Endpoint        feed[2]{};  // destinations; multicast or unicast
    net::Ipv4            iface = net::kLoopback;
    net::Endpoint        rewind{};    // UDP bind address
    net::Endpoint        snapshot{};  // TCP bind address
    std::string          symbol = "SYM1";
    u32                  rewind_capacity = 1u << 16;
    replay::StreamConfig stream[2]{};  // session is taken from above
    u64                  start_delay_ns = 100'000'000;
    u64                  eos_interval_ns = 50'000'000;
    u32                  eos_repeats = 10;
    bool                 faults_on = false;
    bool                 trailer = false;    // append a measurement trailer to feed datagrams
    bool                 busy_wait = false;  // spin between packets instead of sleeping in poll()
    SendFaults           faults{};
};

struct FeedServerStats {
    u64 sent[2] = {0, 0};
    u64 dropped[2] = {0, 0};
    u64 duplicated[2] = {0, 0};
    u64 send_errors = 0;
    u64 messages = 0;
    u64 rewind_served = 0;
    u64 rewind_unavailable = 0;
    u64 snapshots_served = 0;
    u64 snapshots_cut = 0;
    u64 max_lag_ns = 0;   // how far behind schedule a packet left
    u64 book_digest = 0;  // the snapshot server's book at the end: what a receiver must match
};

class FeedServer {
public:
    // The sockets are bound here, so a caller can read the chosen ports before
    // starting run().
    FeedServer(const FeedServerConfig& cfg, std::unique_ptr<AnySource> a,
               std::unique_ptr<AnySource> b);
    ~FeedServer();

    [[nodiscard]] net::Endpoint rewind_endpoint() const;
    [[nodiscard]] net::Endpoint snapshot_endpoint() const;

    // Sends everything, then end of session eos_repeats times, serving requests
    // throughout. Returns early if stop becomes true.
    FeedServerStats run(const std::atomic<bool>& stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ttt::live

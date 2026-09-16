// The receiver, on real sockets: feed A and feed B, a rewind client and a
// snapshot client around the same LineHandler the chaos harness drives.
//
// This is the portable baseline: one thread, poll(), and recvfrom on plain
// sockets. It exists to run the full recovery path over a real network stack on
// any machine. The receive paths that are measured on Linux replace the socket
// loop, never the handler.
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "itch/core/types.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/line_handler.hpp"
#include "ttt/measure/histogram.hpp"
#include "ttt/net/endpoint.hpp"
#include "ttt/net/socket.hpp"

namespace ttt::live {

using itch::u64;

struct ReceiverConfig {
    feed::LineConfig   line{};
    net::Endpoint      feed[2]{};  // what to bind: a multicast group, or a unicast address
    net::Ipv4          iface = net::kLoopback;
    net::Endpoint      rewind_server{};
    net::Endpoint      snapshot_server{};
    std::string        symbol = "SYM1";
    feed::BookCapacity book{};
    int                rcvbuf = 4 << 20;
    u64                idle_timeout_ns = 5'000'000'000;  // give up if nothing arrives this long
    bool               trailer = false;         // feed datagrams carry a measurement trailer
    bool               record_latency = false;  // needs trailer
};

// Latency as this receiver saw it, all on the host's monotonic clock. Only
// meaningful when sender and receiver share that clock, i.e. the same machine.
struct ReceiverLatency {
    measure::Histogram intended_to_recv;  // the schedule to recvfrom returning
    measure::Histogram sent_to_recv;      // the sender's send call to recvfrom returning
    measure::Histogram sender_lag;        // how late the sender was against its schedule
    measure::Histogram handler;           // recvfrom returning to the handler returning
    u64                without_trailer = 0;
};

struct ReceiverResult {
    feed::LineStats                line{};
    u64                            applied = 0;  // sequence number of the last message in the book
    u64                            digest = 0;   // Project 1's book digest of the final book
    bool                           ended = false;
    bool                           stale = true;
    u64                            snapshot_connects = 0;
    u64                            snapshot_failures = 0;
    std::string                    stopped_because;
    std::optional<ReceiverLatency> latency;
};

class Receiver {
public:
    explicit Receiver(const ReceiverConfig& cfg);
    ~Receiver();

    // Where the feed sockets ended up bound (useful when a port was 0).
    [[nodiscard]] net::Endpoint feed_endpoint(int f) const;

    // Where to send retransmission requests and fetch snapshots, if they were
    // not known when the receiver was built.
    void set_servers(const net::Endpoint& rewind, const net::Endpoint& snapshot);

    // Runs until end of session has been seen and the book is complete, or
    // until the idle timeout, or until stop becomes true.
    ReceiverResult run(const std::atomic<bool>& stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ttt::live

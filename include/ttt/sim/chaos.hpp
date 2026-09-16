// The chaos harness: the whole feed path in one process, in simulated time,
// with every fault drawn from one seed.
//
//   generator -> PacketStream A --faulty channel--+
//             -> PacketStream B --faulty channel--+--> LineHandler --> receiver book
//             -> RewindServer   <--requests, faulty responses--+           |
//             -> SnapshotServer <--requests, flaky snapshots---+       compared with
//                                                                      the oracle book
//
// Feed A and feed B carry the same messages packed into different packet sizes,
// so arbitration has to work at message level. Channels drop, duplicate, delay
// and reorder packets, lose them in Gilbert-Elliott bursts, and go dark together
// in joint outages that force retransmission. The rewind server forgets old
// messages and loses responses; snapshots fail outright. Faults stop when the
// stream ends, and end of session is repeated after that, so every run has to
// converge.
//
// The oracle is a second book fed the clean stream. After every message the
// receiver applies, the oracle is advanced to the same sequence number and the
// two are compared; after every step and after every snapshot the full book
// digest is compared, which covers price-time priority and not only depth. The
// claim being tested is not "the book ends up right" but "the book is never
// wrong".
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "itch/core/types.hpp"
#include "ttt/feed/line_handler.hpp"
#include "ttt/sim/order_gen.hpp"

namespace ttt::sim {

using itch::u32;
using itch::u64;

struct ChannelFaults {
    u32 drop_ppm = 0;
    u32 dup_ppm = 0;
    u64 base_delay_ns = 5'000;
    u64 jitter_ns = 0;               // uniform extra delay; more than the packet spacing reorders
    u32 burst_enter_ppm = 0;         // Gilbert-Elliott, per packet: good -> bad
    u32 burst_exit_ppm = 1'000'000;  // bad -> good; while bad, everything is lost
};

struct ChaosConfig {
    u64         seed = 0;
    GenConfig   gen{};
    std::string symbol = "SYM1";

    std::size_t budget_a = 1472;
    std::size_t budget_b = 1472;
    u64         flush_window_ns = 50'000;
    u64         heartbeat_ns = 20'000'000;

    ChannelFaults                    feed[2]{};
    std::vector<std::pair<u64, u64>> joint_outages;  // [from, to) in stream time

    u32 rewind_capacity = 1u << 16;
    u32 request_drop_ppm = 0;
    u32 response_drop_ppm = 0;
    u64 rewind_delay_ns = 50'000;

    u32 snapshot_fail_ppm = 0;
    u64 snapshot_delay_ns = 2'000'000;

    feed::LineConfig line{};
    u32              eos_repeats = 20;
    u64              quiet_limit_ns = 60'000'000'000;  // give up converging after this
};

// The fault profile for a seed. Most dimensions are drawn independently, with
// deliberate weight on the extremes (a dead feed, a rewind server that never
// answers, a buffer of eight slots) because that is where recovery bugs live.
[[nodiscard]] ChaosConfig random_config(u64 seed);

struct ChaosResult {
    bool        ok = true;
    std::string failure;  // the first violation, if any

    u64             messages = 0;
    u64             events = 0;
    u64             message_checks = 0;
    u64             digest_checks = 0;
    u64             end_ns = 0;
    feed::LineStats line{};
    u64             retransmits_served = 0;
    u64             retransmits_unavailable = 0;
    u64             snapshots_built = 0;
};

[[nodiscard]] ChaosResult run(const ChaosConfig& cfg);

}  // namespace ttt::sim

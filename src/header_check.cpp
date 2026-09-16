// Compiles every header under the strict project warning set. Tests include
// them too, but with lighter warnings.
#include "ttt/audit/sequence_audit.hpp"
#include "ttt/core/clock.hpp"
#include "ttt/core/mutant.hpp"
#include "ttt/core/rng.hpp"
#include "ttt/feed/book_sink.hpp"
#include "ttt/feed/line_handler.hpp"
#include "ttt/feed/seq_slots.hpp"
#include "ttt/feed/snapshot_wire.hpp"
#include "ttt/itch/encode.hpp"
#include "ttt/live/feed_server.hpp"
#include "ttt/live/receiver.hpp"
#include "ttt/mold/mold.hpp"
#include "ttt/mold/packer.hpp"
#include "ttt/net/endpoint.hpp"
#include "ttt/net/socket.hpp"
#include "ttt/pcap/pcap.hpp"
#include "ttt/replay/limited.hpp"
#include "ttt/replay/packet_stream.hpp"
#include "ttt/replay/servers.hpp"
#include "ttt/sim/chaos.hpp"
#include "ttt/sim/order_gen.hpp"

// LineHandler is a template; instantiate it once here so the strict warning set
// sees its body, not only its declaration.
namespace {
struct NullActions {
    void apply(itch::u64, std::span<const std::byte>) {}
    void begin_snapshot(itch::u64) {}
    void apply_snapshot(std::span<const std::byte>) {}
    void end_snapshot(itch::u64) {}
    void request_retransmit(itch::u64, itch::u16) {}
    void request_snapshot() {}
};
}  // namespace
template class ttt::feed::LineHandler<NullActions>;

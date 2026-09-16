// Planted bugs, so the chaos checker can be shown to catch real mistakes.
//
// A checker that has never caught anything proves nothing. Each mutant is one
// plausible bug in the receive path or the snapshot server, compiled in only
// under the `mutants` preset and switched on at run time by name. The mutants
// test runs the chaos harness once per mutant and requires every run to fail.
//
// In every other build TTT_MUTANT(x) is the constant false, so the bug paths
// are dead code and cost nothing.
#pragma once

#include <array>
#include <string_view>

#include "itch/core/types.hpp"

namespace ttt::mutant {

enum class Id : itch::u8 {
    None,
    OverlapMisnumbered,     // a packet straddling the frontier is numbered from the frontier
    ApplyDuplicates,        // messages already applied are applied again
    IgnoreControlFrontier,  // heartbeats and end of session never reveal a tail gap
    SlotSeqUnchecked,       // a buffered slot is trusted without checking its sequence
    SnapshotLabelOffByOne,  // the server labels a snapshot one message too early
    SnapshotReversedQueue,  // the server lists each level's orders back to front
};

struct Named {
    Id               id;
    std::string_view name;
};

inline constexpr std::array<Named, 6> kAll{{
    {Id::OverlapMisnumbered, "OverlapMisnumbered"},
    {Id::ApplyDuplicates, "ApplyDuplicates"},
    {Id::IgnoreControlFrontier, "IgnoreControlFrontier"},
    {Id::SlotSeqUnchecked, "SlotSeqUnchecked"},
    {Id::SnapshotLabelOffByOne, "SnapshotLabelOffByOne"},
    {Id::SnapshotReversedQueue, "SnapshotReversedQueue"},
}};

#ifdef TTT_MUTANTS
inline Id& active() noexcept {
    static Id id = Id::None;
    return id;
}
#define TTT_MUTANT(name) (::ttt::mutant::active() == ::ttt::mutant::Id::name)
#else
#define TTT_MUTANT(name) false
#endif

}  // namespace ttt::mutant

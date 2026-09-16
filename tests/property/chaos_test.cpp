// The chaos harness as a test. Every configuration runs a range of seeds; the
// big runs are done with ttt_chaos and recorded in DESIGN.md.
#include "ttt/sim/chaos.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace ttt::sim {
namespace {

u64 seeds_from_env(u64 fallback) {
    const char* v = std::getenv("TTT_CHAOS_SEEDS");
    return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

TEST(Chaos, BookIsNeverWrongAndAlwaysConverges) {
    const u64 n = seeds_from_env(300);
    u64       snapshots = 0, retransmits = 0, gaps = 0;
    for (u64 seed = 1; seed <= n; ++seed) {
        const ChaosResult r = run(random_config(seed));
        ASSERT_TRUE(r.ok) << "seed " << seed << ": " << r.failure;
        snapshots += r.line.snapshots_applied;
        retransmits += r.line.retransmit_requests;
        gaps += r.line.gaps_opened;
    }
    // The run has to have exercised recovery at all, or passing means nothing.
    EXPECT_GT(gaps, n);
    EXPECT_GT(retransmits, 0u);
    EXPECT_GT(snapshots, 0u);
}

TEST(Chaos, SameSeedSameRun) {
    for (u64 seed : {3u, 17u, 99u}) {
        const ChaosResult a = run(random_config(seed));
        const ChaosResult b = run(random_config(seed));
        EXPECT_EQ(a.events, b.events);
        EXPECT_EQ(a.end_ns, b.end_ns);
        EXPECT_EQ(a.line.retransmit_requests, b.line.retransmit_requests);
        EXPECT_EQ(a.line.snapshots_applied, b.line.snapshots_applied);
        EXPECT_EQ(a.digest_checks, b.digest_checks);
    }
}

}  // namespace
}  // namespace ttt::sim

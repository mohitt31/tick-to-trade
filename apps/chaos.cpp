// Runs the chaos harness over a range of seeds.
//
// Usage:
//   ttt_chaos [--seeds N] [--first S] [--mutant NAME] [--keep-going] [--verbose]
//
// Stops at the first failing seed unless --keep-going. Exit status 0 only if
// every seed passed. With --mutant (mutants preset only), a planted bug is
// switched on; the mutants test requires this to fail.

#include "ttt/sim/chaos.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "itch/core/assert.hpp"
#include "ttt/core/mutant.hpp"

using namespace ttt;
using itch::u64;

namespace {

u64 g_seed = 0;

// A book assertion aborts the process. Say which seed did it first, so the
// failure can be replayed.
void report_seed(const itch::detail::AssertInfo& info) {
    std::fprintf(stderr, "chaos: assertion failed on seed %" PRIu64 ": %s (%s:%d)%s%s\n", g_seed,
                 info.expr, info.file, info.line, info.msg != nullptr ? " -- " : "",
                 info.msg != nullptr ? info.msg : "");
}

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr, "ttt_chaos: %s\nsee the comment at the top of apps/chaos.cpp\n", why);
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    u64  seeds = 1000;
    u64  first = 1;
    bool keep_going = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto                   val = [&]() -> const char* {
            if (i + 1 >= argc) usage("option needs a value");
            return argv[++i];
        };
        if (a == "--seeds") {
            seeds = std::strtoull(val(), nullptr, 10);
        } else if (a == "--first") {
            first = std::strtoull(val(), nullptr, 10);
        } else if (a == "--keep-going") {
            keep_going = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--mutant") {
            const std::string_view name = val();
#ifdef TTT_MUTANTS
            bool found = false;
            for (const auto& m : mutant::kAll) {
                if (m.name == name) {
                    mutant::active() = m.id;
                    found = true;
                }
            }
            if (!found) usage("unknown mutant");
#else
            (void)name;
            usage("--mutant needs the mutants preset");
#endif
        } else {
            usage("unknown option");
        }
    }

    itch::detail::set_assert_handler(&report_seed);

    u64 failed = 0;
    u64 messages = 0, gaps = 0, retransmits = 0, snapshots = 0, evicted = 0, duplicates = 0;
    u64 checks = 0, digests = 0;
    for (u64 s = first; s < first + seeds; ++s) {
        g_seed = s;
        const sim::ChaosResult r = sim::run(sim::random_config(s));
        messages += r.messages;
        gaps += r.line.gaps_opened;
        retransmits += r.line.retransmit_requests;
        snapshots += r.line.snapshots_applied;
        evicted += r.line.evicted;
        duplicates += r.line.duplicates;
        checks += r.message_checks;
        digests += r.digest_checks;
        if (verbose || !r.ok) {
            std::printf("seed %" PRIu64 ": %s  messages %" PRIu64 " gaps %" PRIu64
                        " retransmits %" PRIu64 " snapshots %" PRIu64 " evicted %" PRIu64 "%s%s\n",
                        s, r.ok ? "ok" : "FAILED", r.messages, r.line.gaps_opened,
                        r.line.retransmit_requests, r.line.snapshots_applied, r.line.evicted,
                        r.ok ? "" : "  -- ", r.failure.c_str());
        }
        if (!r.ok) {
            ++failed;
            if (!keep_going) {
                break;
            }
        }
    }
    std::printf("seeds %" PRIu64 "..%" PRIu64 "  failed %" PRIu64 "\n", first, first + seeds - 1,
                failed);
    std::printf("messages %" PRIu64 "  per-message checks %" PRIu64 "  digest checks %" PRIu64 "\n",
                messages, checks, digests);
    std::printf("gaps %" PRIu64 "  retransmit requests %" PRIu64 "  snapshots applied %" PRIu64
                "  evictions %" PRIu64 "  duplicates dropped %" PRIu64 "\n",
                gaps, retransmits, snapshots, evicted, duplicates);
    return failed == 0 ? 0 : 1;
}

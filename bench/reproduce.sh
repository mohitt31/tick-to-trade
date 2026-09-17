#!/usr/bin/env bash
# Regenerates every result the README states.
#
#   bench/reproduce.sh correctness [ITCH_FILE]   any machine: build, tests, chaos, replay audit
#   bench/reproduce.sh box                       the Linux box, as root, in the order of
#                                                docs/box-runbook.md (sections 3 to 7)
#
# Correctness results are machine independent and can be rerun anywhere. Box
# results are not: every one of them carries its machine manifest, and a number
# goes in NUMBERS.md only with that manifest and the command below it.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-}

case "$mode" in
correctness)
    itch=${2:-${ITCH_FILE:-}}
    cmake --preset release > /dev/null
    cmake --build --preset release
    ctest --preset release
    echo "== chaos, 100,000 seeds"
    build/release/apps/ttt_chaos --seeds 100000 --keep-going
    if [ -n "$itch" ]; then
        echo "== replay the first 2,000,000 messages at 100k packets/s, twice, and audit"
        tmp=$(mktemp -d)
        build/release/apps/ttt_replay --input "$itch" --pcap "$tmp/a.pcap" --rate 100000 --max-messages 2000000
        build/release/apps/ttt_replay --input "$itch" --pcap "$tmp/b.pcap" --rate 100000 --max-messages 2000000 > /dev/null
        cmp "$tmp/a.pcap" "$tmp/b.pcap" && echo "the two pcaps are byte-identical"
        build/release/apps/ttt_pcap_audit "$tmp/a.pcap"
        rm -rf "$tmp"
    else
        echo "(no ITCH file given: skipping the replay audit)"
    fi
    cmake --preset mutants > /dev/null
    cmake --build --preset mutants
    ctest --preset mutants -R mutant
    ;;
box)
    [ "$(uname -s)" = Linux ] || { echo "box mode runs on the Linux box" >&2; exit 2; }
    : "${ITCH_FILE:?set ITCH_FILE}"
    tools/box/setup_check.sh
    tools/box/gate0_nic.sh
    tools/box/paths.sh 5
    tools/box/summarize.py measurements/paths --baseline recvfrom > docs/receive-paths.md
    for i in $(seq 10); do tools/box/ablation.sh baseline 1; done
    for k in fifo mlock irq_affinity cstates governor thp_never thp_always; do tools/box/ablation.sh "$k" 5; done
    tools/box/summarize.py measurements/ablation > docs/tuning-ablation.md
    tools/box/chaos_wire.sh 60
    echo "Boot knobs (isolcpus, nohz_full, rcu_nocbs, cstates_boot) need a reboot each: see docs/box-runbook.md."
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac

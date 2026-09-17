#!/usr/bin/env bash
# The chaos test on the real wire: A/B feeds, retransmission and snapshots over
# the cable, faults injected at the sender, for as long as asked.
#
#   tools/box/chaos_wire.sh MINUTES [MESSAGES]
#
# Each iteration picks its faults from its seed (loss, duplication, reordering
# jitter, joint outages given as sequence ranges, lost retransmission replies,
# snapshots cut halfway), runs ttt_recv against ttt_feedd, and passes only if the
# receiver saw end of session with a complete book whose digest equals the
# sender's own. The first failure keeps its seed and both outputs; the run goes
# on, and the summary counts passes and failures.
#
# Output: measurements/chaos-wire-<time>/summary.txt, plus failing iterations.
source "$(dirname "$0")/common.sh"
need_root
need_bin ttt_recv
need_bin ttt_feedd
MINUTES=${1:?minutes}
MESSAGES=${2:-200000}
RATE=${RATE:-20000}
out="$TTT_OUT/chaos-wire-$(stamp)"
mkdir -p "$out"
end=$(( $(date +%s) + MINUTES * 60 ))
pass=0
fail=0
seed=0

while [ "$(date +%s)" -lt "$end" ]; do
    seed=$((seed + 1))
    RANDOM=$seed
    drop=$(( RANDOM % 50000 ))           # up to 5% per feed
    dup=$(( RANDOM % 20000 ))
    jitter=$(( (RANDOM % 3) * 500000 ))  # 0, 0.5 or 1 ms: enough to reorder
    short=$(( 1000 + RANDOM % (MESSAGES / 2) ))
    long=$(( MESSAGES / 2 + RANDOM % (MESSAGES / 4) ))
    faults=(--faults "$seed:$drop:$dup:$jitter"
            --outage "$short:$((short + 50 + RANDOM % 300))"
            --outage "$long:$((long + 5000 + RANDOM % 5000))"
            --response-drop-ppm $(( RANDOM % 300000 ))
            --snapshot-cut-ppm $(( RANDOM % 400000 )))

    taskset -c "$RX_CPU" "$TTT_BIN/ttt_recv" --feed-a "$RX_ADDR:$FEED_PORT" --feed-b "$RX_ADDR:$FEED_B_PORT" \
        --iface "$RX_ADDR" --rewind "$SENDER_ADDR:31001" --snapshot "$SENDER_ADDR:31002" \
        --idle-timeout-ms 10000 > "$out/recv.txt" 2>&1 &
    rx=$!
    sleep 0.5
    sender --generate "$seed:$MESSAGES" --rate "$RATE" --budget-a 1400 --budget-b 600 \
        --start-delay-ms 300 "${faults[@]}" > "$out/feedd.txt" 2>&1 || true
    rc=0; wait $rx || rc=$?

    want=$(awk '/^book digest/ {print $4}' "$out/feedd.txt")
    got=$(awk '/^book digest/ {print $4}' "$out/recv.txt")
    if [ $rc -eq 0 ] && [ -n "$want" ] && [ "$want" = "$got" ]; then
        pass=$((pass + 1))
        say "seed $seed pass  $(grep -E 'retransmit requests' "$out/recv.txt" | sed 's/  */ /g')"
    else
        fail=$((fail + 1))
        say "seed $seed FAIL  recv exit $rc, digest sender ${want:-none} receiver ${got:-none}"
        cp "$out/recv.txt" "$out/fail-seed$seed-recv.txt"
        cp "$out/feedd.txt" "$out/fail-seed$seed-feedd.txt"
        say "  faults: ${faults[*]}" | tee -a "$out/fail-seed$seed-feedd.txt"
    fi
done

rm -f "$out/recv.txt" "$out/feedd.txt"
say "iterations $((pass + fail))  pass $pass  fail $fail  (${MINUTES} min, $MESSAGES messages each, $RATE pps)" \
    | tee "$out/summary.txt"
[ $fail -eq 0 ]

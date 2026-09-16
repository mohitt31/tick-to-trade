#!/usr/bin/env bash
# One tuning knob, measured on its own against the stock baseline.
#
#   tools/box/ablation.sh KNOB [RUNS]
#
# KNOB is one of:
#   baseline                 nothing changed (run this 10 times first: it is the noise floor)
#   isolcpus nohz_full rcu_nocbs cstates_boot   boot knobs: reboot with the parameter first
#   fifo mlock irq_affinity cstates governor thp_never thp_always
#
# For each knob it runs both receive modes (a thread that sleeps: blocking
# recvmmsg; a thread that never does: busy-polled recvmmsg) at a low and a high
# rate, RUNS times each. Several knobs only matter in one of the two modes, so
# both are always measured. Everything not being ablated stays stock, and the
# receiver is always pinned to RX_CPU, including in the baseline.
#
# Results land in measurements/ablation/<knob>/<mode>-<rate>pps/run<N>-*.hgrm,
# each with the machine manifest on top. tools/box/summarize.py turns them into
# the table in docs/tuning-ablation.md.
source "$(dirname "$0")/common.sh"
need_root
need_bin ttt_rxbench
need_bin ttt_feedd
KNOB=${1:?knob}
RUNS=${2:-5}
RATES=${RATES:-"10000 500000"}
SECONDS_PER_RUN=${SECONDS_PER_RUN:-60}
WARMUP_MS=${WARMUP_MS:-10000}
ITCH_FILE=${ITCH_FILE:?set ITCH_FILE to the NASDAQ ITCH day file}
K="$(dirname "$0")/knobs.sh"

rx_flags=()
boot_params=""
case "$KNOB" in
baseline) ;;
isolcpus) boot_params="isolcpus=$RX_CPU" ;;
nohz_full) boot_params="nohz_full=$RX_CPU" ;;
rcu_nocbs) boot_params="rcu_nocbs=$RX_CPU" ;;
cstates_boot) boot_params="intel_idle.max_cstate=0 processor.max_cstate=1" ;;
fifo) rx_flags+=(--fifo 50) ;;
mlock) rx_flags+=(--mlock) ;;
irq_affinity) "$K" set irqs "$TX_CPU" ;;   # interrupts away from the receiver's core
cstates) "$K" set cstates shallow ;;
governor) "$K" set governor performance ;;
thp_never) "$K" set thp never ;;
thp_always) "$K" set thp always ;;
*) die "unknown knob $KNOB" ;;
esac
[ -n "$boot_params" ] && "$K" boot-check "$boot_params"
# fifo on a spinning thread needs RT throttling off, or it is throttled for 50 ms
# of every second, which is a knob of its own; record that it was changed.
[ "$KNOB" = fifo ] && "$K" set rt-throttle off

restore() { [ "$KNOB" != baseline ] && [ -z "$boot_params" ] && "$K" restore-stock > /dev/null || true; }
trap restore EXIT

for mode in blocking busy; do
    mode_flags=()
    [ "$mode" = busy ] && mode_flags=(--busy-poll-us 50 --prefer-busy-poll)
    for rate in $RATES; do
        dir="$TTT_OUT/ablation/$KNOB/$mode-${rate}pps"
        mkdir -p "$dir"
        # Enough packets for warmup plus the measured window, at about 40
        # messages a packet.
        messages=$(( rate * (SECONDS_PER_RUN + WARMUP_MS / 1000) * 40 ))
        for run in $(seq "$RUNS"); do
            say "== $KNOB $mode $rate pps run $run"
            "$K" show > "$dir/run$run-knobs.txt"
            taskset -c "$RX_CPU" "$TTT_BIN/ttt_rxbench" --path recvmmsg:32 --feed "$RX_ADDR:$FEED_PORT" \
                --iface "$RX_ADDR" --session TTTLIVE --manifest-iface "$RX_IF" --warmup-ms "$WARMUP_MS" \
                --latency-out "$dir/run$run" --idle-timeout-ms 5000 "${mode_flags[@]}" "${rx_flags[@]}" \
                > "$dir/run$run-rx.txt" 2>&1 &
            rx=$!
            sleep 1
            sender --input "$ITCH_FILE" --max-messages "$messages" --rate "$rate" --budget-a 1400 \
                --budget-b 1400 --trailer --busy-wait --preload --no-servers --start-delay-ms 500 > "$dir/run$run-tx.txt" 2>&1
            wait $rx || say "   receiver reported loss or an incomplete book: see $dir/run$run-rx.txt"
            grep -E 'applied|intended_to_recv' "$dir/run$run-rx.txt" | sed 's/^/   /' || true
        done
    done
done

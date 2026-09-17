#!/usr/bin/env bash
# The five-way comparison: every receive path under the same conditions, with
# the counters that explain the differences.
#
#   tools/box/paths.sh [RUNS]
#
# Each path is run RUNS times at each rate in RATES. perf stat counts, for the
# receiver process:
#   raw_syscalls:sys_enter, context-switches, cpu-migrations, page-faults
#   probe:skb_consume_udp   a datagram leaving a UDP socket after its one copy to
#                           user memory: once per datagram on every socket path
# and, system-wide over the same run, because AF_XDP does its work in softirq
# context and not in the receiver's process:
#   probe:__xsk_rcv         AF_XDP copy mode copying a packet into the UMEM
#   probe:__xsk_rcv_zc      a frame put on the RX ring. Despite the name this runs
#                           in copy mode too; zero-copy shows as __xsk_rcv == 0
# Divided by datagrams, those are the system calls and copies per packet each
# step toward AF_XDP zero-copy removes. Chosen after checking on Linux 6.12:
# skb_copy_datagram_iter barely fires for UDP (linear skbs are copied inline) and
# _copy_to_iter cannot be probed. Kernel function names change, so the script
# adds whichever exist, records which, and removes them afterwards.
#
# Results: measurements/paths/<path>/<rate>pps/run<N>-{*.hgrm,perf.csv,rx.txt,tx.txt}.
# Summarize with: tools/box/summarize.py measurements/paths --baseline recvfrom
source "$(dirname "$0")/common.sh"
need_root
need_bin ttt_rxbench
need_bin ttt_feedd
command -v perf > /dev/null || die "perf is needed for the counters"
RUNS=${1:-5}
RATES=${RATES:-"10000 100000"}
BUDGET=${BUDGET:-300}
SECONDS_PER_RUN=${SECONDS_PER_RUN:-60}
WARMUP_MS=${WARMUP_MS:-10000}
ITCH_FILE=${ITCH_FILE:?set ITCH_FILE to the NASDAQ ITCH day file}
PATHS=${PATHS:-"recvfrom recvmmsg:1 recvmmsg:8 recvmmsg:32 recvmmsg:128 epoll-lt epoll-et uring uring-sqpoll uring-defer xdp:$RX_IF:0:native:copy xdp:$RX_IF:0:native:zerocopy xdp:$RX_IF:0:native:zerocopy:busy"}

mountpoint -q /sys/kernel/tracing || mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
probes=()
for fn in skb_consume_udp __xsk_rcv __xsk_rcv_zc; do
    # Kprobes live in the kernel, not the process: one left from an earlier run
    # makes adding it again fail, which must not read as "no such function".
    perf probe -q -d "probe:$fn" 2>/dev/null || true
    if perf probe -q -a "$fn" 2>/dev/null; then probes+=("probe:$fn"); else say "no probe for $fn on this kernel"; fi
done
cleanup() { for p in "${probes[@]}"; do perf probe -q -d "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
events="raw_syscalls:sys_enter,context-switches,cpu-migrations,page-faults"
sys_events=""
for p in "${probes[@]}"; do
    if [ "$p" = probe:skb_consume_udp ]; then events+=",$p"; else sys_events+="${sys_events:+,}$p"; fi
done
mkdir -p "$TTT_OUT/paths"
say "probes: ${probes[*]:-none}" | tee "$TTT_OUT/paths/probes.txt"

for path in $PATHS; do
    for rate in $RATES; do
        dir="$TTT_OUT/paths/$path/${rate}pps"
        mkdir -p "$dir"
        # About ten messages a packet at a 300-byte budget.
        messages=$(( rate * (SECONDS_PER_RUN + WARMUP_MS / 1000) * 10 ))
        for run in $(seq "$RUNS"); do
            say "== $path $rate pps run $run"
            if [ -n "$sys_events" ]; then
                perf stat -a -x, -o "$dir/run$run-perf-sys.csv" -e "$sys_events" -- \
                    sleep $((SECONDS_PER_RUN + WARMUP_MS / 1000 + 12)) &
                sys_perf=$!
            fi
            perf stat -x, -o "$dir/run$run-perf.csv" -e "$events" -- \
                taskset -c "$RX_CPU" "$TTT_BIN/ttt_rxbench" --path "$path" --feed "$RX_ADDR:$FEED_PORT" \
                --iface "$RX_ADDR" --manifest-iface "$RX_IF" --warmup-ms "$WARMUP_MS" \
                --latency-out "$dir/run$run" --idle-timeout-ms 5000 > "$dir/run$run-rx.txt" 2>&1 &
            rx=$!
            sleep 2  # XDP attach and socket setup before traffic
            sender --input "$ITCH_FILE" --max-messages "$messages" --rate "$rate" --budget-a "$BUDGET" \
                --budget-b "$BUDGET" --trailer --busy-wait --preload --no-servers --start-delay-ms 500 \
                > "$dir/run$run-tx.txt" 2>&1 || say "   sender failed: see $dir/run$run-tx.txt"
            wait $rx || say "   receiver reported loss or an incomplete book: see $dir/run$run-rx.txt"
            [ -n "${sys_perf:-}" ] && { kill -INT "$sys_perf" 2>/dev/null; wait "$sys_perf" 2>/dev/null || true; }
            grep -E '^applied|^syscalls|intended_to_recv' "$dir/run$run-rx.txt" | sed 's/^/   /' || true
        done
    done
done

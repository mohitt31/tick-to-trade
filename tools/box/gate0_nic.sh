#!/usr/bin/env bash
# Gate 0: before any number, prove the receive NIC does native XDP and AF_XDP
# zero-copy, and that nothing is lost at any rate we will measure at.
#
#   tools/box/topology.sh up
#   tools/box/gate0_nic.sh [copy]     # "copy" only to rehearse on hardware without zero-copy
#
# Steps, each PASS or FAIL, all written to measurements/gate0-<host>-<time>.txt:
#   1  driver, firmware, kernel, channels, offloads, timestamping capabilities
#   2  queue steering: RX_IF must have exactly one combined channel, so the
#      feed cannot land on a queue with no socket
#   3  ttt_xdp_probe: forced native attach and forced zero-copy bind, both read
#      back from the kernel, with live traffic
#   4  loss by sequence number at a sweep of rates, through the real AF_XDP
#      zero-copy path, with the NIC's and socket's counters as second witnesses
#   5  hardware receive timestamps: rx filter "all" is offered
#
# A FAIL at 3 or 4 stops the AF_XDP part of the project until it is understood.
source "$(dirname "$0")/common.sh"
need_root
need_bin ttt_xdp_probe
need_bin ttt_rxbench
need_bin ttt_feedd
BIND=${1:-zerocopy}
RATES=${RATES:-"10000 100000 500000"}
MESSAGES=${MESSAGES:-2000000}
mkdir -p "$TTT_OUT"
out="$TTT_OUT/gate0-$(host)-$(stamp).txt"
fails=0
result() { printf '%-28s %s  %s\n' "$1" "$2" "$3"; [ "$2" = PASS ] || fails=$((fails + 1)); }

{
say "# gate 0 on $(host), $(date -u), kernel $(uname -r), bind mode $BIND"
say
say "## 1. the interface"
ethtool -i "$RX_IF" || true
ethtool -l "$RX_IF" || true
ethtool -k "$RX_IF" | grep -E 'generic-receive-offload|large-receive-offload|rx-checksumming|rx-vlan-offload' || true
ethtool -T "$RX_IF" || true
ethtool -c "$RX_IF" || true
ethtool --show-eee "$RX_IF" 2>/dev/null | head -3 || true
say

say "## 2. queue steering"
combined=$(ethtool -l "$RX_IF" 2>/dev/null | awk '/Current hardware settings/ {c=1} c && /Combined/ {print $2; exit}')
queues=$(ls -d /sys/class/net/$RX_IF/queues/rx-* 2>/dev/null | wc -l)
if [ "${combined:-$queues}" = 1 ] || [ "$queues" = 1 ]; then
    result "one rx queue" PASS "combined=${combined:-n/a} rx queues=$queues"
else
    result "one rx queue" FAIL "combined=${combined:-n/a} rx queues=$queues (ethtool -L $RX_IF combined 1)"
fi
say

say "## 3. forced modes, read back, with traffic"
( sleep 1; sender --generate 1:200000 --rate 20000 --budget-a 1400 --budget-b 1400 --start-delay-ms 500 > /dev/null ) &
if taskset -c "$RX_CPU" "$TTT_BIN/ttt_xdp_probe" --iface "$RX_IF" --queue 0 --attach native --bind "$BIND" \
    --match "$RX_ADDR:$FEED_PORT" --listen-ms 4000; then
    result "probe" PASS "native attach and $BIND bind read back, traffic seen"
else
    result "probe" FAIL "see the probe output above"
fi
wait
say

say "## 4. loss by sequence number"
for rate in $RATES; do
    before=$(ethtool -S "$RX_IF" 2>/dev/null | grep -iE 'drop|miss|error|fifo|no_buf' | sort || true)
    taskset -c "$RX_CPU" "$TTT_BIN/ttt_rxbench" --path "xdp:$RX_IF:0:native:$BIND" \
        --feed "$RX_ADDR:$FEED_PORT" --idle-timeout-ms 3000 > /tmp/gate0-rx.txt 2>&1 &
    rx=$!
    sleep 1
    sender --generate 7:"$MESSAGES" --rate "$rate" --budget-a 1400 --budget-b 1400 --trailer --busy-wait --preload --no-servers --start-delay-ms 300 > /tmp/gate0-tx.txt 2>&1
    rc=0; wait $rx || rc=$?
    after=$(ethtool -S "$RX_IF" 2>/dev/null | grep -iE 'drop|miss|error|fifo|no_buf' | sort || true)
    grep -E 'stopped|applied|xdp program|xsk statistics|syscalls' /tmp/gate0-rx.txt || true
    grep -E 'sent A|max lag' /tmp/gate0-tx.txt || true
    say "nic counters that moved:"
    diff <(say "$before") <(say "$after") | grep '^>' || say "  none"
    if [ $rc -eq 0 ]; then
        result "no loss at $rate pps" PASS "every message by sequence, no program or socket drops"
    else
        result "no loss at $rate pps" FAIL "see above"
    fi
done
say

say "## 5. hardware receive timestamps"
if ethtool -T "$RX_IF" 2>/dev/null | grep -qE '^\s*all\s'; then
    result "rx filter all" PASS "every received packet can be hardware timestamped"
else
    result "rx filter all" FAIL "no HWTSTAMP_FILTER_ALL: external timing needs a second machine"
fi
say
if [ $fails -eq 0 ]; then say "GATE 0 PASS"; else say "GATE 0 FAIL ($fails)"; fi
} 2>&1 | tee "$out"
say "wrote $out"
grep -q "GATE 0 PASS" "$out"

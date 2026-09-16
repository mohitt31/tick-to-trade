#!/usr/bin/env bash
# Puts the sender port in its own network namespace so one box can send to
# itself over a real cable: p1 (namespace) -> cable -> p2 (root).
#
#   tools/box/topology.sh up        use the physical ports SENDER_IF and RX_IF
#   tools/box/topology.sh up --veth a veth pair instead, for trying the scripts
#                                   anywhere (no cable, no real NIC)
#   tools/box/topology.sh down
#   tools/box/topology.sh show
#
# Without the namespace the kernel would see both addresses as local and route
# the packets over loopback, never touching the wire.
source "$(dirname "$0")/common.sh"
need_root

case "${1:-}" in
up)
    ip netns add "$SENDER_NS" 2>/dev/null || true
    if [ "${2:-}" = "--veth" ]; then
        ip link add "$RX_IF" type veth peer name "$SENDER_IF"
    fi
    ip link set "$SENDER_IF" netns "$SENDER_NS"
    ip addr replace "$RX_ADDR/24" dev "$RX_IF"
    ip link set "$RX_IF" up
    ip -n "$SENDER_NS" addr replace "$SENDER_ADDR/24" dev "$SENDER_IF"
    ip -n "$SENDER_NS" link set "$SENDER_IF" up
    ip -n "$SENDER_NS" link set lo up
    # Wait for carrier, then prove the path with ARP.
    for _ in $(seq 50); do
        [ "$(cat /sys/class/net/$RX_IF/carrier 2>/dev/null)" = 1 ] && break
        sleep 0.1
    done
    ip netns exec "$SENDER_NS" ping -c 2 -W 1 "$RX_ADDR" > /dev/null || die "no path $SENDER_ADDR -> $RX_ADDR"
    say "up: $SENDER_NS/$SENDER_IF $SENDER_ADDR -> $RX_IF $RX_ADDR"
    ;;
down)
    ip -n "$SENDER_NS" link set "$SENDER_IF" netns 1 2>/dev/null || true
    ip netns del "$SENDER_NS" 2>/dev/null || true
    ip link del "$RX_IF" 2>/dev/null || true  # removes a veth pair; a physical port stays
    say "down"
    ;;
show)
    ip -br addr show dev "$RX_IF" || true
    ip -n "$SENDER_NS" -br addr 2>/dev/null || say "no namespace $SENDER_NS"
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac

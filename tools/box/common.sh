# Shared settings for the box scripts. Source it; override any of these in the
# environment. The defaults describe the one-box rig: port p1 in a sender
# namespace, cabled to port p2 in the root namespace, where the receiver runs.
set -euo pipefail

TTT_ROOT=${TTT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
TTT_BIN=${TTT_BIN:-$TTT_ROOT/build/linux-release/apps}
TTT_OUT=${TTT_OUT:-$TTT_ROOT/measurements}

SENDER_NS=${SENDER_NS:-ttt_tx}      # namespace the sender port lives in
SENDER_IF=${SENDER_IF:-p1}          # sender port (moved into SENDER_NS)
RX_IF=${RX_IF:-p2}                  # receiver port
SENDER_ADDR=${SENDER_ADDR:-10.77.0.2}
RX_ADDR=${RX_ADDR:-10.77.0.1}
FEED_PORT=${FEED_PORT:-30001}
FEED_B_PORT=${FEED_B_PORT:-30002}
RX_CPU=${RX_CPU:-7}                 # isolated core for the receiver
TX_CPU=${TX_CPU:-3}                 # core for the sender

stamp() { date -u +%Y%m%dT%H%M%SZ; }
host() { hostname -s; }
say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || die "run as root"; }
need_bin() { [ -x "$TTT_BIN/$1" ] || die "$TTT_BIN/$1 not built (cmake --preset release -B build/linux-release)"; }

# Runs the feed server in the sender namespace, pinned. Arguments are passed on.
sender() {
    ip netns exec "$SENDER_NS" taskset -c "$TX_CPU" "$TTT_BIN/ttt_feedd" \
        --iface "$SENDER_ADDR" --feed-a "$RX_ADDR:$FEED_PORT" --feed-b "$RX_ADDR:$FEED_B_PORT" \
        --rewind "$SENDER_ADDR:31001" --snapshot "$SENDER_ADDR:31002" "$@"
}

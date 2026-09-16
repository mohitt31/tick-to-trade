#!/usr/bin/env bash
# What this machine is, and whether it has what the project needs. Run once when
# the box arrives, and again after any kernel or BIOS change. Writes
# measurements/setup-<host>-<time>.txt.
source "$(dirname "$0")/common.sh"
mkdir -p "$TTT_OUT"
out="$TTT_OUT/setup-$(host)-$(stamp).txt"

{
    say "## machine"
    uname -a
    grep -m1 PRETTY_NAME /etc/os-release || true
    lscpu | grep -E 'Model name|^CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|NUMA node|MHz' || true
    say "kernel cmdline: $(cat /proc/cmdline)"
    say "rmem_max $(cat /proc/sys/net/core/rmem_max)  busy_poll $(cat /proc/sys/net/core/busy_poll)"

    say; say "## network interfaces with a device"
    for d in /sys/class/net/*; do
        n=$(basename "$d")
        [ -e "$d/device" ] || continue
        drv=$(basename "$(readlink "$d/device/driver")")
        say "$n driver=$drv speed=$(cat "$d/speed" 2>/dev/null || echo ?) queues=$(ls -d "$d"/queues/rx-* 2>/dev/null | wc -l) numa=$(cat "$d/device/numa_node" 2>/dev/null || echo ?)"
        ethtool -i "$n" 2>/dev/null | grep -E 'driver|version|firmware|bus-info' | sed 's/^/    /'
        ethtool -T "$n" 2>/dev/null | grep -E 'hardware-receive|all|PTP Hardware Clock' | sed 's/^/    /'
    done

    say; say "## tools"
    for t in perf bpftool ethtool tcpdump turbostat rtla numastat cpupower hwstamp_ctl phc2sys xdp-loader irqbalance chrt taskset; do
        if command -v "$t" > /dev/null; then say "have    $t"; else say "MISSING $t"; fi
    done
    systemctl is-active irqbalance 2>/dev/null | sed 's/^/irqbalance: /' || true
} | tee "$out"
say "wrote $out"

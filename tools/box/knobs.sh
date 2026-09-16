#!/usr/bin/env bash
# Reads and sets the kernel tuning knobs, one at a time. Every "set" is followed
# by reading the knob back from the kernel; a set that does not stick fails.
#
#   tools/box/knobs.sh show                    every knob, as the kernel has it now
#   tools/box/knobs.sh save-stock              record the current state as stock (once, untuned)
#   tools/box/knobs.sh restore-stock           put runtime knobs back to stock
#   tools/box/knobs.sh set governor performance
#   tools/box/knobs.sh set epp performance
#   tools/box/knobs.sh set thp never|madvise|always
#   tools/box/knobs.sh set cstates shallow|stock   hold /dev/cpu_dma_latency at 0, or release it
#   tools/box/knobs.sh set irqs CPULIST        all RX_IF interrupts to these CPUs (stops irqbalance)
#   tools/box/knobs.sh set rt-throttle off|stock
#   tools/box/knobs.sh boot-check "isolcpus=7 nohz_full=7"   the running kernel has these parameters
#
# Per-process knobs (CPU affinity, SCHED_FIFO, mlockall, busy poll) are flags on
# ttt_rxbench, not here. Boot knobs need a reboot: add them to the kernel command
# line, reboot, then boot-check refuses to continue unless they are really there.
source "$(dirname "$0")/common.sh"
STOCK=$TTT_OUT/stock-knobs.env
DMA_PID=/run/ttt-cpu-dma-latency.pid
cpus() { ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V; }
first() { cat "$(cpus | head -1)/$1" 2>/dev/null || echo n/a; }
bracketed() { sed -n 's/.*\[\(.*\)\].*/\1/p' "$1" 2>/dev/null || echo n/a; }
irqs() { awk -v ifc="$RX_IF" '$0 ~ ifc {sub(":", "", $1); print $1}' /proc/interrupts; }

show() {
    say "cmdline=$(cat /proc/cmdline)"
    say "isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
    say "nohz_full=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
    say "rcuo_threads=$(pgrep -c '^rcuo' || true)"
    say "governor=$(first cpufreq/scaling_governor)"
    say "epp=$(first cpufreq/energy_performance_preference)"
    say "cpuidle_driver=$(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo n/a)"
    say "cstates_held=$([ -f $DMA_PID ] && kill -0 "$(cat $DMA_PID)" 2>/dev/null && echo yes || echo no)"
    say "thp=$(bracketed /sys/kernel/mm/transparent_hugepage/enabled)"
    say "rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us)"
    say "irqbalance=$(systemctl is-active irqbalance 2>/dev/null || echo n/a)"
    for i in $(irqs); do
        say "irq_$i=allowed:$(cat /proc/irq/$i/smp_affinity_list) effective:$(cat /proc/irq/$i/effective_affinity_list 2>/dev/null)"
    done
}

set_all_cpus() {  # file value
    for c in $(cpus); do
        [ -w "$c/$1" ] && echo "$2" > "$c/$1"
    done
    [ "$(first "$1")" = "$2" ] || die "$1 did not stick: $(first "$1")"
}

case "${1:-}" in
show) show ;;
save-stock)
    need_root; mkdir -p "$TTT_OUT"
    [ -e "$STOCK" ] && die "$STOCK exists; stock is recorded once, before any tuning"
    { say "governor=$(first cpufreq/scaling_governor)"; say "epp=$(first cpufreq/energy_performance_preference)"
      say "thp=$(bracketed /sys/kernel/mm/transparent_hugepage/enabled)"; say "rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us)"
      say "irqbalance=$(systemctl is-active irqbalance 2>/dev/null || echo inactive)"; } > "$STOCK"
    cat "$STOCK"
    ;;
restore-stock)
    need_root; [ -e "$STOCK" ] || die "no $STOCK; run save-stock on the untuned box first"
    # shellcheck disable=SC1090
    source "$STOCK"
    [ "$governor" != n/a ] && set_all_cpus cpufreq/scaling_governor "$governor"
    [ "$epp" != n/a ] && set_all_cpus cpufreq/energy_performance_preference "$epp"
    echo "$thp" > /sys/kernel/mm/transparent_hugepage/enabled
    echo "$rt_runtime_us" > /proc/sys/kernel/sched_rt_runtime_us
    "$0" set cstates stock
    [ "$irqbalance" = active ] && systemctl start irqbalance
    show
    ;;
set)
    need_root
    case "${2:-}" in
    governor) set_all_cpus cpufreq/scaling_governor "$3" ;;
    epp) set_all_cpus cpufreq/energy_performance_preference "$3" ;;
    thp)
        echo "$3" > /sys/kernel/mm/transparent_hugepage/enabled
        [ "$(bracketed /sys/kernel/mm/transparent_hugepage/enabled)" = "$3" ] || die "thp did not stick"
        ;;
    cstates)
        if [ -f $DMA_PID ]; then kill "$(cat $DMA_PID)" 2>/dev/null || true; rm -f $DMA_PID; fi
        if [ "$3" = shallow ]; then
            # The request holds only while the file stays open: a background
            # process keeps it open, writing a 0 us latency limit as a binary s32.
            nohup bash -c 'exec 3>/dev/cpu_dma_latency; printf "\x00\x00\x00\x00" >&3; exec sleep infinity' \
                > /dev/null 2>&1 &
            echo $! > $DMA_PID
            sleep 0.2
            kill -0 "$(cat $DMA_PID)" || die "cannot hold /dev/cpu_dma_latency"
        fi
        ;;
    irqs)
        systemctl stop irqbalance 2>/dev/null || true
        for i in $(irqs); do echo "$3" > /proc/irq/$i/smp_affinity_list || say "irq $i refused $3"; done
        ;;
    rt-throttle)
        if [ "$3" = off ]; then echo -1 > /proc/sys/kernel/sched_rt_runtime_us; else echo 950000 > /proc/sys/kernel/sched_rt_runtime_us; fi
        ;;
    *) die "unknown knob ${2:-}" ;;
    esac
    show | grep -E "^${2%%-*}|^irq|^cstates|^rt_runtime" || true
    ;;
boot-check)
    for p in ${2:-}; do
        grep -qw -- "$p" /proc/cmdline || die "the running kernel was not booted with $p"
    done
    say "boot parameters present: ${2:-}"
    ;;
*) sed -n '2,20p' "$0"; exit 2 ;;
esac

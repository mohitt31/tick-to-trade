#!/usr/bin/env bash
# Records what happened on the receiver's core while it measured, so the slow
# datagrams can be matched to their causes.
#
#   tools/box/jitter_record.sh OUT_DIR -- <ttt_rxbench arguments, including --latency-out and --outliers-over-ns>
#
# While the receiver runs (start the sender separately), this records on RX_CPU:
#   perf       IRQ and softirq entry, user page faults, task migration, context
#              switches, hrtimer expiry, tick stop, workqueue work, all stamped
#              with CLOCK_MONOTONIC, the same clock the receiver's outlier log
#              uses, so the two timelines join directly
#   vmstat     compaction and THP counters, sampled every 100 ms
#   turbostat  SMI count per second. SMIs are invisible to every kernel tracer.
#              If outliers line up with SMIs and nothing else, this is the only
#              place that shows it
#
# Then run tools/box/jitter_correlate.py on OUT_DIR.
source "$(dirname "$0")/common.sh"
need_root
need_bin ttt_rxbench
OUT=${1:?output directory}; shift
[ "${1:-}" = "--" ] && shift
mkdir -p "$OUT"

EVENTS=irq:irq_handler_entry,irq:softirq_entry,exceptions:page_fault_user,sched:sched_migrate_task,sched:sched_switch,timer:hrtimer_expire_entry,timer:tick_stop,workqueue:workqueue_execute_start

perf record -q -k CLOCK_MONOTONIC -C "$RX_CPU" -e "$EVENTS" -o "$OUT/perf.data" &
perf_pid=$!
python3 - "$OUT/vmstat.csv" <<'PY' &
import sys, time
keys = ("compact_stall", "compact_fail", "compact_success", "thp_fault_alloc", "thp_collapse_alloc", "pgfault", "pgmajfault")
with open(sys.argv[1], "w") as out:
    out.write("monotonic_ns," + ",".join(keys) + "\n")
    while True:
        vals = {}
        with open("/proc/vmstat") as f:
            for line in f:
                k, v = line.split()
                vals[k] = v
        out.write(str(time.clock_gettime_ns(time.CLOCK_MONOTONIC)) + "," + ",".join(vals.get(k, "") for k in keys) + "\n")
        out.flush()
        time.sleep(0.1)
PY
vm_pid=$!
if command -v turbostat > /dev/null; then
    turbostat --quiet --show SMI,Busy%,CPU --cpu "$RX_CPU" --interval 1 > "$OUT/turbostat.txt" 2>&1 &
    ts_pid=$!
fi
sleep 1

rc=0
taskset -c "$RX_CPU" "$TTT_BIN/ttt_rxbench" "$@" > "$OUT/rx.txt" 2>&1 || rc=$?

kill -INT $perf_pid; wait $perf_pid 2>/dev/null || true
kill $vm_pid 2>/dev/null || true
[ -n "${ts_pid:-}" ] && kill $ts_pid 2>/dev/null || true
perf script -i "$OUT/perf.data" -F comm,pid,cpu,time,event,trace --ns > "$OUT/perf.txt" 2>/dev/null
say "receiver exit $rc; wrote $OUT/{rx.txt,perf.data,perf.txt,vmstat.csv,turbostat.txt}"

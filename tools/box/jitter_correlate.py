#!/usr/bin/env python3
"""Matches each slow datagram with the kernel events just before it.

    tools/box/jitter_correlate.py OUTLIERS.csv PERF.txt [--window-us 2000] [--cpu 7]

OUTLIERS.csv is ttt_rxbench's outlier log; PERF.txt is `perf script --ns` output
recorded with -k CLOCK_MONOTONIC on the receiver's core (tools/box/jitter_record.sh
produces both). Both use the monotonic clock, so an event and an outlier can be
compared directly.

An event "explains" an outlier if it happened on the core within the window
before the datagram was received, the window being at least as long as the
outlier's own delay. The report gives, per event type, the share of outliers
with at least one such event, and the share with none of any kind: the
unexplained remainder, which is reported rather than hidden. Sharing a window
with an outlier does not prove an event caused it, so the table is where the
hunt starts, and the fix and a before/after run are what prove the cause.

--selftest checks the matching against a hand-made case.
"""
import argparse
import bisect
import collections
import re
import sys

# "comm pid [cpu] seconds.nanoseconds: subsystem:event: fields"
LINE = re.compile(r"\[(\d+)\]\s+(\d+)\.(\d{9}):\s+(\w+:\w+):\s*(.*)$")


def read_outliers(path):
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or line.startswith("recv_"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 5:
                out.append((int(parts[0]), int(parts[1]), int(parts[2]), int(parts[4])))
    return out  # (recv_ns, seq, intended_to_recv_ns, handled_ns)


def read_events(lines, cpu):
    events = []
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        if cpu is not None and int(m.group(1)) != cpu:
            continue
        t = int(m.group(2)) * 1_000_000_000 + int(m.group(3))
        name = m.group(4)
        detail = m.group(5)
        if name == "irq:irq_handler_entry":
            irq = re.search(r"name=(\S+)", detail)
            name = f"irq:{irq.group(1)}" if irq else name
        elif name == "irq:softirq_entry":
            vec = re.search(r"action=(\S+)", detail)
            name = f"softirq:{vec.group(1)}" if vec else name
        elif name == "sched:sched_switch":
            nxt = re.search(r"next_comm=(\S+)", detail)
            name = f"switch_to:{nxt.group(1)}" if nxt else name
        elif name == "timer:hrtimer_expire_entry":
            fn = re.search(r"function=(\S+)", detail)
            name = f"hrtimer:{fn.group(1)}" if fn else name
        elif name == "workqueue:workqueue_execute_start":
            fn = re.search(r"function (\S+)", detail)
            name = f"work:{fn.group(1)}" if fn else name
        events.append((t, name))
    events.sort()
    return events


def correlate(outliers, events, window_ns):
    times = [t for t, _ in events]
    per_type = collections.Counter()
    unexplained = 0
    for recv_ns, _seq, delay_ns, handled_ns in outliers:
        span = max(window_ns, delay_ns, handled_ns)
        lo = bisect.bisect_left(times, recv_ns - span)
        hi = bisect.bisect_right(times, recv_ns)
        kinds = {events[i][1] for i in range(lo, hi)}
        if not kinds:
            unexplained += 1
        per_type.update(kinds)
    return per_type, unexplained


def selftest():
    outliers = [(10_000_000, 1, 500_000, 600_000), (50_000_000, 2, 300_000, 400_000), (90_000_000, 3, 200_000, 300_000)]
    perf = [
        "ttt_rxbench 42 [007]     0.009800000: irq:irq_handler_entry: irq=31 name=igc-rx-0",
        "swapper 0 [007]     0.049900000: timer:hrtimer_expire_entry: hrtimer=0x0 function=tick_nohz_handler now=1",
        "kworker 9 [003]     0.089900000: workqueue:workqueue_execute_start: work struct 0x0: function vmstat_update",
    ]
    per_type, unexplained = correlate(outliers, read_events(perf, 7), 1_000_000)
    assert per_type == {"irq:igc-rx-0": 1, "hrtimer:tick_nohz_handler": 1}, per_type
    assert unexplained == 1, unexplained  # the vmstat work ran on CPU 3, not the receiver's
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outliers", nargs="?")
    ap.add_argument("perf", nargs="?")
    ap.add_argument("--window-us", type=int, default=2000)
    ap.add_argument("--cpu", type=int, default=None)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.outliers or not args.perf:
        ap.error("OUTLIERS.csv and PERF.txt are required")

    outliers = read_outliers(args.outliers)
    with open(args.perf) as f:
        events = read_events(f, args.cpu)
    if not outliers:
        sys.exit("no outliers in the log")
    per_type, unexplained = correlate(outliers, events, args.window_us * 1000)
    n = len(outliers)
    print(f"{n} outliers, {len(events)} events, window {args.window_us} us (or the outlier's own delay if longer)\n")
    print("| event on the receiver's core | outliers with it | share |")
    print("|---|---|---|")
    for name, count in per_type.most_common(25):
        print(f"| {name} | {count} | {100.0 * count / n:.1f}% |")
    print(f"| (nothing) | {unexplained} | {100.0 * unexplained / n:.1f}% |")


if __name__ == "__main__":
    main()

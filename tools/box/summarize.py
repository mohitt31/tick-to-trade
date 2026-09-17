#!/usr/bin/env python3
"""Turns ablation or benchmark .hgrm files into a markdown table.

    tools/box/summarize.py measurements/ablation [--histogram intended_to_recv]

Groups files by the directories above them (knob, then mode and rate), takes each
run's p50, p99 and p99.9 from the percentile table, and reports the median across
runs with the min..max spread. Against the baseline group it prints the change in
the median and whether that change is larger than the baseline's own spread,
which is the only honest meaning of "this knob did something".

Runs whose receiver output reports missing messages are left out and counted,
because a run that lost data measured something else.

When a run has perf counters next to it (tools/box/paths.sh writes runN-perf.csv),
a second table gives system calls, copies and context switches per datagram, the
median across runs: the mechanism behind the latency table. Copies are
skb_consume_udp (socket paths) plus __xsk_rcv (AF_XDP copy mode); the AF_XDP rx
ring column is __xsk_rcv_zc, which runs in both AF_XDP modes, so zero-copy is
copies == 0 with rx ring == 1, not the other way round.
"""
import argparse
import os
import re
import statistics
import sys
from collections import defaultdict

PCTS = (50.0, 99.0, 99.9)


def read_hgrm(path):
    manifest, rows = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith("# ") and ": " in line:
                k, v = line[2:].rstrip("\n").split(": ", 1)
                manifest[k] = v
                continue
            parts = line.split()
            if len(parts) >= 3:
                try:
                    rows.append((float(parts[0]), float(parts[1])))  # value in us, percentile 0..1
                except ValueError:
                    pass
    return manifest, rows


def at(rows, pct):
    want = pct / 100.0
    for value, p in rows:
        if p >= want:
            return value
    return rows[-1][0] if rows else float("nan")


def run_stem(hgrm_path):
    return re.sub(r"-[a-z_]+\.hgrm$", "", hgrm_path)


def per_datagram(hgrm_path):
    """Counters per datagram for one run, or None without perf data."""
    stem = run_stem(hgrm_path)
    perf, sys_perf, rx = stem + "-perf.csv", stem + "-perf-sys.csv", stem + "-rx.txt"
    if not (os.path.exists(perf) and os.path.exists(rx)):
        return None
    with open(rx) as f:
        m = re.search(r"datagrams (\d+)", f.read())
    if not m or int(m.group(1)) == 0:
        return None
    n = int(m.group(1))
    counts = defaultdict(float)
    for name in (perf, sys_perf):
        if not os.path.exists(name):
            continue
        with open(name) as f:
            for line in f:
                parts = line.strip().split(",")
                if len(parts) < 3 or not parts[0] or parts[0].startswith("<"):
                    continue
                try:
                    counts[parts[2]] += float(parts[0])
                except ValueError:
                    pass
    def per(*events):
        # None when no listed event was counted at all: a counter that was not
        # collected is not a zero.
        present = [e for e in events if e in counts]
        return sum(counts[e] for e in present) / n if present else None

    return {
        "syscalls": per("raw_syscalls:sys_enter"),
        "copies": per("probe:skb_consume_udp", "probe:__xsk_rcv"),
        "rx_ring": per("probe:__xsk_rcv_zc"),
        "context_switches": per("context-switches"),
    }


def lost(hgrm_path):
    rx = run_stem(hgrm_path) + "-rx.txt"
    if not os.path.exists(rx):
        return False
    with open(rx) as f:
        m = re.search(r"missing (\d+)", f.read())
    return bool(m and int(m.group(1)) != 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--histogram", default="intended_to_recv")
    ap.add_argument("--baseline", default="baseline")
    args = ap.parse_args()

    groups = defaultdict(list)  # (knob, case) -> [ {pct: us} ]
    counters = defaultdict(list)  # (knob, case) -> [ per-datagram dicts ]
    commits, skipped = set(), 0
    for dirpath, _, files in os.walk(args.root):
        for name in sorted(files):
            if not name.endswith(f"-{args.histogram}.hgrm"):
                continue
            path = os.path.join(dirpath, name)
            if lost(path):
                skipped += 1
                continue
            manifest, rows = read_hgrm(path)
            if not rows:
                continue
            commits.add(manifest.get("git_commit", "?"))
            rel = os.path.relpath(dirpath, args.root).split(os.sep)
            knob, case = rel[0], "/".join(rel[1:]) or "-"
            groups[(knob, case)].append({p: at(rows, p) for p in PCTS})
            c = per_datagram(path)
            if c is not None:
                counters[(knob, case)].append(c)

    if not groups:
        sys.exit(f"no *-{args.histogram}.hgrm files under {args.root}")

    def summary(runs):
        out = {}
        for p in PCTS:
            vals = [r[p] for r in runs]
            out[p] = (statistics.median(vals), min(vals), max(vals))
        return out

    print(f"Histogram `{args.histogram}`, microseconds. Median of runs, with min..max.")
    print(f"Commits: {', '.join(sorted(commits))}. Runs left out for lost messages: {skipped}.\n")
    print("| knob | case | runs | p50 | p99 | p99.9 | p99 vs baseline |")
    print("|---|---|---|---|---|---|---|")
    for (knob, case) in sorted(groups, key=lambda k: (k[1], k[0] != args.baseline, k[0])):
        runs = groups[(knob, case)]
        s = summary(runs)
        cells = [f"{s[p][0]:.1f} ({s[p][1]:.1f}..{s[p][2]:.1f})" for p in PCTS]
        verdict = ""
        base = groups.get((args.baseline, case))
        if base and knob != args.baseline:
            b = summary(base)[99.0]
            delta = s[99.0][0] - b[0]
            beyond = s[99.0][0] < b[1] or s[99.0][0] > b[2]
            verdict = f"{delta:+.1f} ({'beyond noise' if beyond else 'within noise'})"
        print(f"| {knob} | {case} | {len(runs)} | " + " | ".join(cells) + f" | {verdict} |")

    if counters:
        print("\nPer datagram, median of runs (from perf stat on the receiver process).\n")
        print("| path | case | runs | syscalls | copies | AF_XDP rx ring | context switches |")
        print("|---|---|---|---|---|---|---|")
        for key in sorted(counters, key=lambda k: (k[1], k[0])):
            rs = counters[key]
            def med(k):
                vals = [r[k] for r in rs if r[k] is not None]
                return f"{statistics.median(vals):.3f}" if vals else "n/a"

            print(f"| {key[0]} | {key[1]} | {len(rs)} | {med('syscalls')} | {med('copies')} | "
                  f"{med('rx_ring')} | {med('context_switches')} |")


if __name__ == "__main__":
    main()

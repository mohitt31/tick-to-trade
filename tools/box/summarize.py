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


def lost(hgrm_path):
    stem = re.sub(r"-[a-z_]+\.hgrm$", "", hgrm_path)
    rx = stem + "-rx.txt"
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


if __name__ == "__main__":
    main()

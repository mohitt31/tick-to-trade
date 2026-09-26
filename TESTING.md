# Test verification

A full run of the project's test suite on native Linux. Every preset, the
chaos harness, and the CI script checks pass with no failures.

## Environment

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS |
| Compiler | GCC 13.3.0 (`g++`) |
| Build | CMake 3.28.3, Ninja 1.11.1 |
| Kernel | 6.18.44 |
| Libraries | liburing 2.5, libbpf 1.3.0, libxdp 1.4.2, zlib |
| Submodule | `third_party/itch-exchange` @ `f52e3df` |

Built and tested exactly as the README describes:

```sh
for p in release asan-ubsan tsan mutants; do
  cmake --preset $p -DCMAKE_CXX_COMPILER=g++
  cmake --build --preset $p
  ctest --preset $p
done
```

## Results

| Preset | Tests | Result | Time |
|--------|-------|--------|------|
| `release` | 71 | **71 passed, 0 failed** | 10.5 s |
| `asan-ubsan` | 71 | **71 passed, 0 failed** — no ASan/UBSan reports | 29.0 s |
| `tsan` | 71 | **71 passed, 0 failed** — no data races | 44.2 s |
| `mutants` | 78 | **78 passed, 0 failed** | 17.4 s |

The `mutants` preset adds seven tests to the base 71: one per planted bug plus a
clean control. All six planted bugs are caught by the chaos harness, and the
control run is clean:

```
mutant.OverlapMisnumbered ...... Passed
mutant.ApplyDuplicates ......... Passed
mutant.IgnoreControlFrontier ... Passed
mutant.SlotSeqUnchecked ........ Passed
mutant.SnapshotLabelOffByOne ... Passed
mutant.SnapshotReversedQueue ... Passed
mutant.control ................. Passed
```

### Chaos harness

100,000 seeds, no failures:

```
seeds 1..100000  failed 0
messages 153000485  per-message checks 112290120  digest checks 76408919
gaps 754181  retransmit requests 460373  snapshots applied 80326
evictions 20341700  duplicates dropped 68357344
```

### CI `scripts` job

- `bash -n` on all 11 shell scripts under `tools/box/`, `tools/`, and `bench/` — all pass.
- `python3 tools/box/jitter_correlate.py --selftest` — `selftest ok`.

## Linux receive paths and AF_XDP

The Linux-only paths, which only build on Linux, were exercised in full and
passed — including under ASan and TSan:

- **io_uring / epoll** — `RxPaths.IoUringAllModes`, `RxPaths.OutlierLog`, and the
  rest of the `RxPaths.*` suite.
- **AF_XDP** — the `VethPair.*` tests created a real `veth` pair and network
  namespace and drove the feed through copy mode, verified that forced
  zero-copy fails rather than silently falling back, and checked RX timestamp
  metadata and per-queue packet accounting.

## Notes

Two build inputs were sourced differently for this environment; no project
source was modified:

- The HdrHistogram dependency is fetched as a release tarball by
  `FetchContent`. Outbound access to the tarball host was blocked here, so it
  was supplied from a `git clone` of the same pinned tag `0.11.10` via
  `-DFETCHCONTENT_SOURCE_DIR_HDR_HISTOGRAM`.
- `find_program` resolved `bpftool` to a version-detecting wrapper that could
  not match the running kernel. The build was pointed at the working
  `bpftool` v7.4.0 binary via `-DBPFTOOL`.

TSan needs reduced mmap randomness on recent kernels, applied as the CI does:
`sysctl -w vm.mmap_rnd_bits=28`.

## Out of scope for this environment

These need other hardware and are not test gaps in the code:

- **macOS matrix** — the CI also runs all four presets on macOS; a Mac is
  required.
- **Performance numbers** — per the README, these come only from bare metal and
  are not published yet. `bench/reproduce.sh` also needs a real NASDAQ ITCH
  capture file.
- **AF_XDP zero-copy on a physical NIC** — a real NIC is required; the
  zero-copy failure path is covered above over `veth`.

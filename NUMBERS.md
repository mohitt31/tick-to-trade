# Numbers

Every number in the README appears here with the machine it was measured on,
the build flags, and the exact command that reproduces it. Nothing here is an
estimate.

## Correctness runs

These do not depend on the machine's speed and can be rerun anywhere with
`bench/reproduce.sh correctness`.

**Chaos harness, 100,000 seeds, no failures.** Apple M4, macOS 26.5, Apple clang
21, `release` preset (`-O2 -g`), commit e87af32.

```
build/release/apps/ttt_chaos --seeds 100000 --keep-going
seeds 1..100000  failed 0
messages 153000485  per-message checks 112290120  digest checks 76408919
gaps 754181  retransmit requests 460373  snapshots applied 80326  evictions 20341700  duplicates dropped 68357344
```

**Six planted bugs, each caught.** Same machine, `mutants` preset:
`ctest --preset mutants -R mutant`. First failing seed per bug is in DESIGN.md
section 9.

**Replay of the real file is deterministic and pristine.** Same machine,
`release` preset, commit ef1d3d2: the first 2,000,000 messages of
`01302019.NASDAQ_ITCH50.gz` at 100,000 packets a second give 40,416 packets, two
runs produce byte-identical pcaps, and the audit reports every message exactly
once, in order.

```
build/release/apps/ttt_replay --input 01302019.NASDAQ_ITCH50.gz --pcap a.pcap --rate 100000 --max-messages 2000000
build/release/apps/ttt_pcap_audit a.pcap
```

## Performance

Nothing measured yet. Measurements come only from the Linux box, after Gate 0
passes, and each will carry its machine manifest.

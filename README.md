# tick-to-trade

The receive side of a market data path: NASDAQ ITCH 5.0 carried over MoldUDP64
multicast, received through five different Linux receive paths up to AF_XDP
zero-copy, with A/B feed arbitration, gap recovery and snapshot
resynchronisation feeding the order book from
[itch-exchange](https://github.com/mohitt31/itch-exchange).

What exists so far runs without a network: the MoldUDP64 codec, a replayer that
turns an ITCH file into paced packets (written to pcap and checked with an
audit), A/B arbitration with retransmission and snapshot recovery, and a chaos
harness that checks the book against an oracle after every message under
generated loss, duplication, reordering and outages. The Linux receive paths
come next.

Work in progress. No performance numbers are published yet. When they are, each
one will be in [NUMBERS.md](NUMBERS.md) with the machine it ran on and the
command that reproduces it.

## Build

```sh
git submodule update --init
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `release`, `asan-ubsan`, `tsan`, `mutants`. The `mutants` preset also
checks that the chaos harness catches each of six planted bugs.

```sh
build/release/apps/ttt_replay --input 01302019.NASDAQ_ITCH50.gz --pcap out.pcap --rate 100000 --max-messages 2000000
build/release/apps/ttt_pcap_audit out.pcap
build/release/apps/ttt_chaos --seeds 100000
```

Design notes are in [DESIGN.md](DESIGN.md).

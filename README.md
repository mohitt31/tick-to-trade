# tick-to-trade

The receive side of a market data path: NASDAQ ITCH 5.0 carried over MoldUDP64
multicast, received through five different Linux receive paths up to AF_XDP
zero-copy, with A/B feed arbitration, gap recovery and snapshot
resynchronisation feeding the order book from
[itch-exchange](https://github.com/mohitt31/itch-exchange).

Work in progress. No performance numbers are published yet. When they are, each
one will be in [NUMBERS.md](NUMBERS.md) with the machine it ran on and the
command that reproduces it.

## Build

```sh
git submodule update --init
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `release`, `asan-ubsan`, `tsan`, `mutants`.

Design notes are in [DESIGN.md](DESIGN.md).

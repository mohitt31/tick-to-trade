# tick-to-trade

The receive side of a market data path. NASDAQ ITCH 5.0 is replayed as MoldUDP64
over UDP, on two feeds, and received through a choice of Linux receive paths:
`recvfrom`, `recvmmsg`, `epoll`, `io_uring` and AF_XDP in copy and zero-copy
mode. Arbitration between the feeds, gap recovery by retransmission and snapshot
resynchronisation feed the order book from
[itch-exchange](https://github.com/mohitt31/itch-exchange).

## What is checked, and how

- **The book is never wrong.** A deterministic chaos harness runs sender, both
  feeds, the rewind and snapshot servers and the receiver in simulated time, with
  every fault drawn from one seed, and compares the receiver's book with an
  oracle after every applied message. 100,000 seeds pass, and each of six planted
  bugs is caught within the first ten seeds.
- **The same code over real sockets.** `ttt_feedd` and `ttt_recv` run the
  pipeline over UDP and TCP with faults injected at the sender, and the
  receiver's final book digest has to equal the sender's.
- **Loss is counted by sequence number**, with the kernel's and the NIC's counters
  only as second witnesses.
- **AF_XDP modes are forced and read back from the kernel**, never left to fall
  back. `ttt_xdp_probe` checks a NIC before anything is measured on it.
- **Copies and system calls per packet are counted with perf**, not asserted.

Performance numbers come only from bare metal and are not published yet. When
they are, each will be in [NUMBERS.md](NUMBERS.md) with its machine and the
command that reproduces it.

## Build

```sh
git submodule update --init
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `release`, `asan-ubsan`, `tsan`, `mutants`. The Linux receive paths and
AF_XDP need liburing, libbpf, clang and bpftool, and build only on Linux. On a Mac
they can be built and tested in `tools/docker_dev.sh`.

```sh
bench/reproduce.sh correctness /path/to/01302019.NASDAQ_ITCH50.gz
```

Running it on the measurement machine is described in
[docs/box-runbook.md](docs/box-runbook.md), and the reasoning behind the design
in [DESIGN.md](DESIGN.md).

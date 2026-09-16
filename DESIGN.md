# Design

Decisions as they were made, with what was rejected and why.

## 1. Where the work runs

Three machines, and every piece of work says which one it belongs to before it
starts.

- **Mac (M4, macOS).** Everything that does not need a Linux kernel or a real
  NIC: the MoldUDP64 codec, the replayer's packing and pacing logic, A/B
  arbitration, gap recovery, the snapshot protocol and the chaos harness. None of
  this produces a latency number.
- **Laptop (x86, native Linux).** Socket receive paths, the kernel tuning
  ablation, generic-mode AF_XDP, software timestamps, the jitter hunt. Its wired
  NIC is a Realtek part on the `r8169` driver, which has no native XDP and no
  hardware timestamping in mainline, so zero-copy cannot be tested there.
- **A box with Intel i226 ports (`igc`).** AF_XDP zero-copy, NIC hardware
  timestamps, and the five-way comparison. `igc` has had AF_XDP zero-copy since
  Linux 5.14 and the i226 timestamps every received packet in hardware.

A number measured on one machine is never put in a table with a number from
another.

## 2. Project 1 as a submodule

The book, the ITCH parser and the replay digest come from `itch-exchange`,
pinned in `third_party/`. This build compiles its headers and its single source
file directly instead of calling `add_subdirectory` on it. Its CMakeLists also
builds its apps, benchmarks and tests and adds `-march=native`, and none of that
belongs here.

`ITCH_INVARIANT_LEVEL` is one cache variable shared by both projects, so the book
is always checked at the same level as the code that drives it.

Rejected: copying the book into this repo. Two copies drift, and the chaos test's
oracle is only worth something if it is the same book that Project 1 tested
differentially against two other implementations.

## 3. Build configurations

`release` is `-O2 -g`. The `-g` costs nothing at run time and `perf annotate` is
useless without it. `asan-ubsan` and `tsan` check at level 2. `mutants` is the
release flags plus planted bugs behind `TTT_MUTANTS`, and it exists so the chaos
checker can be shown to catch them.

## 4. MoldUDP64

The feed is framed as MoldUDP64 because that is how NASDAQ actually carries ITCH
over multicast, and because its sequence numbers are what arbitration and
recovery run on.

**Sequence numbers count messages, not packets.** The header's sequence is the
first message's number. This means feed A and feed B are allowed to split the
same messages into different packets, and the arbiter has to work at message
level. The chaos harness will repacketize one feed on purpose so that this path
actually runs.

**The decoder rejects a packet whole.** It walks every block length before
handing out a single message, and on any error it leaves its output untouched.
Rejected: a streaming decoder that yields messages until it hits a bad block.
Half-applying a corrupt packet puts the book in a state nobody can recover from,
while dropping the packet is just a gap, which the arbiter already knows how to
fill. The cost is a second pass over at most a few dozen length prefixes that
are already in L1.

**Zero-length blocks are rejected.** The specification does not forbid them, but
every ITCH message has at least a type byte, so on this feed an empty block can
only mean corruption.

**The packer owns sequence numbering and nothing else.** It answers "does this
fit" and "close the packet". When to close a packet (byte budget, flush
deadline, burst boundary) is the replayer's policy. Its buffer is allocated once
and reused, so building a packet never allocates.

## 5. The replayer is a pure function

`PacketStream` turns ITCH messages into packets with an intended send time, and
does nothing else. It has no clock and no socket. The same code writes a pcap on
the Mac, will drive the chaos harness in simulated time, and will pace a NIC on
Linux, where the only new piece is the loop that waits for each `send_ns` and
sends. Rejected: a replayer that reads the clock while packing. Its output would
depend on how fast the machine ran, so it could not be tested byte for byte, and
the chaos harness could not replay a seed.

**Fixed rate is an absolute schedule.** Packet `i` is due at `i * 1e9 / pps`,
computed from `i` with 128-bit arithmetic rather than accumulated, so rounding
never drifts. A sender that falls behind cannot quietly stretch the schedule and
make the receiver's latency look better. That guard against coordinated
omission starts here, not in the receiver.

**Feed time closes a packet the way a real sender would.** A packet closes when
the next message does not fit, or when the next message is later than the flush
window allows. A full packet leaves at its last message's time. A packet that is
not full leaves when its flush timer would fire, at the first message's time plus
the window. Silences longer than the heartbeat interval get heartbeats exactly one
interval apart. Timestamps that go backwards are clamped and counted, never
silently reordered.

On the real file (first 5,000,000 messages of 2019-01-30, feed time 1x, 100 µs
window) that coalesces into 2,695,006 packets and 3,354 heartbeats over 4.8
hours of feed time. That is a property of the data at those settings, not a
performance figure:

```
ttt_replay --input 01302019.NASDAQ_ITCH50.gz --pcap feed.pcap --feed-time 1 \
  --flush-window-ns 100000 --heartbeat-ns 1000000000 --max-messages 5000000
```

## 6. The audit trusts sequence numbers and nothing else

`ttt_pcap_audit` reads the replayer's pcap and, later, captures off the real
wire. So the same check covers what was meant to be sent and what arrived. It
counts gaps, duplicates, late packets and overlaps per multicast group, and calls
a flow pristine only if every message arrived exactly once, in order.

**A flow is expected to start at sequence 1.** The first version took the first
packet it saw as the start. A property test (random drops, duplicates and swaps,
checked against `messages + missing == sent`) failed on seed 6: losing the
session's first packet was invisible, because nothing before the first packet
seen was ever expected. A MoldUDP64 session starts at 1, so that is now the
default. A capture joined mid-session has to say so with `--mid-session`.

**A packet behind the frontier is classified by whether it fills a hole.** If
it does, it is late (reordered) and the hole shrinks. If every message in it was
already seen, it is a duplicate. Without this, reordering would be reported as
loss plus duplication, which is a different and wrong diagnosis.

The pcap writer computes real IPv4 and UDP checksums (`tcpdump -vv` reports
`udp sum ok`), and the reader counts bad UDP checksums without dropping the
datagram. A capture taken on the sending host sees unfilled checksums when the
NIC offloads them, and dropping those would turn an offload artefact into fake
loss.

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

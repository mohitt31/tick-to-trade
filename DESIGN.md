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

## 7. Arbitration and recovery: one state machine, no I/O

`LineHandler` takes packets, snapshots and timer expiries with the current time,
and asks for everything else through an Actions object: apply a message, ask for
a retransmission, ask for a snapshot. It is the same shape as the replayer and
for the same reason. The chaos harness drives it in simulated time, and the
Linux receiver will drive it from sockets without changing a line of it.

**First-wins at message level, not packet level.** A message is applied the
first time its sequence number is the next one expected, whichever feed it came
on. Packet-level arbitration (take whichever copy of packet N arrives first)
only works if both feeds split messages into identical packets. MoldUDP64 does
not promise that, and the chaos harness deliberately gives feed B a different
packet budget. A packet straddling the frontier contributes exactly its unseen
tail.

**The frontier comes from control packets too.** A gap is open whenever
something has revealed a sequence number beyond the next expected one. Data
packets reveal it, and so do heartbeats and end of session. Without that, losing
the last packets before a quiet period is invisible. The `IgnoreControlFrontier`
mutant is exactly that bug. The chaos harness catches it on seed 1 as a run that
never converges.

**Escalation.** A gap first waits `arb_wait_ns` for the other feed. Then the
handler asks the rewind server for the first hole only: up to the first message
it already holds, not the whole range to the frontier. Asking for the whole range
re-sends messages already buffered and makes each request larger than it needs
to be. The attempt count starts over whenever the hole moves, because a hole that
moves is progress. A gap larger than `max_retransmit_count`, or one that exhausts
its attempts, goes to a snapshot. Snapshots retry on timeout forever. A receiver
that gives up has no correct state left to fall back to.

**Held messages live in `SeqSlots`, a direct-mapped table keyed by sequence
number.** Slot `s & (C-1)`, one cache line each, and each slot records which
sequence it holds. On a collision the newer message wins. A lookup can miss, but
it can never return the wrong message. The `SlotSeqUnchecked` mutant removes the
sequence check, and the book's own assertions catch it on seed 1. Rejected: a
window relative to the next expected message. It needs an overflow policy, and
during a snapshot, when that message is frozen, everything is outside the window.

**The book is never wrong, only behind.** In-order messages move it forward one
at a time. A snapshot replaces it in one step, and only once the snapshot has
fully decoded. A snapshot older than the book is ignored, never applied
backwards. So at every instant the book is exactly the state after the last
applied message. `stale()` says whether that is also the latest state.

## 8. Snapshots are ITCH

A snapshot is the symbol's stock directory message followed by one Add Order per
resting order, level by level, each level in queue order. The receiver rebuilds
its book by feeding those through the same Project 1 builder it uses for live
data, so there is no second path that could disagree about what a book is.
Queue order is load-bearing: adding orders back in that order is what restores
time priority. The `SnapshotReversedQueue` mutant writes each level back to
front. Depth and quantities are still right, and only the digest that includes
queue order catches it.

The rewind server answers a request only if it holds every message asked for.
A partial answer would look like progress while leaving the hole where it was.

## 9. The chaos harness

`ttt_chaos` runs generator, both feeds, both servers, the receiver and an oracle
book in one process, as a discrete-event simulation where every fault comes from
one seed. Faults per seed are drawn independently with weight on the extremes: a
dead feed, Gilbert-Elliott burst loss, duplication, jitter large enough to
reorder, joint outages on both feeds including one over the tail of the stream,
a rewind server that holds eight messages or never answers, snapshots that fail,
and an arbitration buffer of eight slots. Faults stop when the stream ends and
end of session is repeated, so every run must converge.

**What is checked, and when.** After every message the receiver applies, the
oracle (a second book fed the clean stream) is advanced to the same sequence and
the two are compared on counts, quantities and best prices. After every handler
step and every snapshot, the full book digest is compared, including queue
order. At the end, every message must have been applied, the handler must be
live, and end of session must have been seen. Applying a sequence number out of
order is a failure on the spot.

**The checker has been shown to catch bugs.** Six planted bugs sit behind
`TTT_MUTANT`, compiled only in the `mutants` preset. Each one's test passes only
if the harness actually ran and reported a failing seed, and a control run in the
same build with no bug switched on has to pass 2,000 seeds. Where each was caught
first:

| Mutant | First failing seed | Caught by |
|---|---|---|
| OverlapMisnumbered | 2 | Project 1 book assertion |
| ApplyDuplicates | 2 | sequence check |
| IgnoreControlFrontier | 1 | did not converge |
| SlotSeqUnchecked | 1 | Project 1 book assertion |
| SnapshotLabelOffByOne | 10 | Project 1 book assertion |
| SnapshotReversedQueue | 1 | oracle digest (queue order) |

Three are caught by Project 1's book asserting on an impossible operation before
the oracle comparison runs. That counts as caught, but it means those bugs are
caught because they produce operations the book rejects. A bug that produced
valid but wrong operations would have to be caught by the oracle, and
`SnapshotReversedQueue` is the case that shows the oracle does.

**The full run.** Seeds 1 to 100,000, release build, on the M4:

```
ttt_chaos --seeds 100000 --keep-going
seeds 1..100000  failed 0
messages 153000485  per-message checks 112290120  digest checks 76408919
gaps 754181  retransmit requests 460373  snapshots applied 80326  evictions 20341700  duplicates dropped 68357344
```

Per-message checks are fewer than messages because a snapshot moves the book past
the messages it replaced in one step. Those are covered by the digest check at
the snapshot instead. The same harness runs 3,000 seeds clean under ASan and
UBSan, and `ctest` runs 300 seeds in every preset.

## 10. The pipeline on real sockets

`ttt_feedd` and `ttt_recv` are the chaos harness with the simulated network
replaced by sockets: POSIX UDP for both feeds and for retransmission, TCP for
snapshots, one thread and `poll()` on each side. The same `PacketStream`,
`RewindServer`, `SnapshotServer` and `LineHandler` do the work. Only the loops
around them are new. They build and run on macOS and Linux alike. This
`recvfrom` loop is the portable baseline, not a measured receive path. The Linux
paths replace the socket loop and leave the handler alone.

A receiver binds a multicast feed to the group address, not `INADDR_ANY`, so a
socket for feed A never sees feed B on the same port. Snapshots go over TCP as a
4-byte length and the snapshot. A connection that closes early hands the handler
whatever arrived, and the handler's all-or-nothing decoder rejects it. So a cut
snapshot is handled by the same code path that rejects any other broken one.

**Outages on a real clock are given as sequence ranges, not time windows.** The
first version dropped both feeds for a window of sender time. One run in
fifty-odd failed its coverage check, with no snapshot applied: the sender had
stalled, and the packets due inside the window went out after it closed. A
simulated clock cannot stall and a real one can, so the live server drops any
packet carrying a message in `[from, to)` on both feeds instead. The loopback
tests then place one outage that only a retransmission can fill and one larger
than a retransmission may ask for, so both recovery paths run in every run.
They pass 10 repeats in release, TSan and ASan.

The feed server prints its snapshot book's digest at the end, and the receiver
prints its own. Two processes over loopback multicast, on the first 3,000,000
messages of the 2019-01-30 file with faults on:

```
ttt_recv --symbol AAPL --idle-timeout-ms 5000 &
ttt_feedd --input 01302019.NASDAQ_ITCH50.gz --max-messages 3000000 --symbol AAPL \
  --rate 50000 --budget-b 600 --faults 11:20000:10000:500000 \
  --outage 1000000:1000500 --outage 2000000:2100000 --snapshot-cut-ppm 200000
```

The receiver applied all 3,000,000 messages through 40 gaps, 24 retransmission
requests and 28 snapshots (12 more were cut and rejected), and ended with digest
`504dc49447f624b4`, the server's own. That is a correctness result on loopback,
not a timing one. `max lag` in the server's output is how late its `poll()` loop
sent against schedule, and it is why this loop is not the measurement sender.

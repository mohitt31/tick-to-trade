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

## 11. Measurement plumbing

**Latency is measured against the schedule, and the sender's own lateness is
measured separately.** In measurement mode every feed datagram carries a 32-byte
trailer after the MoldUDP64 packet, holding the packet's intended send time and
the time the sender actually called `sendto`. A receiver on the same host reads
the same clock and records three things: schedule to receive, send to receive,
and sender lag. The first is the coordinated-omission-safe number. The split says
how much of it was the sender. A receiver in trailer mode strips the trailer
before decoding, so the codec stays strict, and nothing outside a measurement run
ever carries one.

A first run on the M4 showed why the split is needed. With `--busy-wait`, the
schedule-to-receive p99 was 2.4 ms, and nearly all of it was sender lag (p99
2.4 ms). Send to receive had a p99 of 50 µs. The sender's loop also feeds the
rewind and snapshot servers, so the book work sits on the send thread. That is
fine for correctness runs and wrong for measurement, and it is what the Linux
measurement sender has to fix. These are Mac figures with no pinning or
isolation, so they are a reason for a design choice and nothing more. None of
them goes in NUMBERS.md.

**The clock is `CLOCK_MONOTONIC_RAW` on macOS and `CLOCK_MONOTONIC` on Linux.**
macOS's `CLOCK_MONOTONIC` advanced in steps of exactly 1000 ns over a million
consecutive readings on the M4, while `CLOCK_MONOTONIC_RAW` stepped at 41 ns.
With the first, every sub-microsecond latency is rounding. A test asserts that
consecutive readings can differ by less than a microsecond.

**HdrHistogram_c, pinned by hash, values recorded as given.** No coordinated
omission correction on top: the schedule already avoids the problem, and
correcting again would double count. Values outside the tracked range are
clamped and counted, and the summary says so.

**Every result file starts with a manifest of what the machine actually was.**
On Linux that is the kernel command line, isolated and nohz_full CPUs, governor
and EPP, idle driver and C-state limit, THP mode, RT throttling, busy-poll
sysctls, maximum temperature and throttle count, the process's CPU affinity and
locked memory, and for the interface its driver, XDP mode, IRQ affinities (allowed
and effective), coalescing and EEE. On macOS it includes Low Power Mode and power
source, because Project 1 found Low Power Mode halves throughput. It always
includes the commit, regenerated on every build with a `-dirty` suffix, so a
number can always be traced to its code. Everything is read from the kernel at
run time, never taken from what a setup script meant to set.

## 12. The Linux receive paths

`ttt_rxbench` measures one path on one feed: `recvfrom`, `recvmmsg:N`,
`epoll-lt`, `epoll-et`, `uring`, `uring-sqpoll`, `uring-defer`. Every path has the
same shape, `receive(on_datagram)`, and one loop (`run_bench`) drives them all,
so between two runs the path is the only thing that changes. Per datagram the
loop stamps the time the call returned, strips the trailer, records latency,
runs the LineHandler and the book, and records how long that took. Datagrams
returned by one call share one timestamp, because that is when user space has
them.

What each path is, so the comparison has something to explain:

- `recvfrom`: blocks in the call, one system call and one copy per datagram.
- `recvmmsg` with `MSG_WAITFORONE`: blocks for the first datagram, takes what
  else is queued in the same call. Same copy, shared call. Its timeout argument is
  only checked between datagrams, so `SO_RCVTIMEO` bounds the wait.
- `epoll`, level-triggered: one read per readiness, back to `epoll_pwait2`
  (nanosecond timeout). Edge-triggered: drain to `EAGAIN`, which costs one empty
  read every wakeup. With a single socket, epoll can only add calls. The tests
  assert the extra calls are there, not that epoll is slower.
- `io_uring`: one multishot `recvmsg` with a 1024-buffer provided ring, so the
  steady state has no per-packet submission. Plain, SQPOLL, and
  `DEFER_TASKRUN | SINGLE_ISSUER`. It is still a socket receive: the network
  stack and the copy remain. zcrx is out, because it needs NIC header split.

Every path counts its own receive calls, wakeups and empty returns. Those explain
the numbers. perf on the box is what confirms them.

These build and pass functional tests in an arm64 Linux container (Linux
6.12, liburing 2.9) on the Mac: all messages arrive, the book digest matches the
oracle, recvmmsg never exceeds its batch, and edge-triggered epoll makes its
extra call. The container is never a measurement.

**Loss is counted two independent ways, and they have to agree.** The first
version reported "lost" as the frontier minus the next expected message. A
single early drop, never filled in a measurement run, made everything behind it
look lost. Loss is now counted by the same sequence audit that checks pcaps,
outside the timed part of each datagram. Separately, `ttt_rxbench` reads the
kernel's UDP counters from `/proc/net/snmp` before and after. The first run that
did both, in the container at 100k packets/s:

```
rcvbuf requested 212992  granted 212992      path uring
missing 475  (by sequence)                   kernel rcvbuf errors 56
```

At about 8.5 messages a packet, 56 dropped datagrams is 475 messages. The two
counts agree, so the loss was the socket buffer and nothing hidden. The cause
was Linux capping `SO_RCVBUF` at `net.core.rmem_max` (212992 there) without
saying so. Asked for 8 MiB, it granted 208 KB. `udp_socket` now tries
`SO_RCVBUFFORCE` first, and `ttt_rxbench` prints what was requested and what
was granted. With 8 MiB granted, every path is complete with zero loss on both
counts. This is the same rule Gate 0 applies to AF_XDP: sequence numbers are
the truth, and a counter is only a second witness.

Knobs that are per-process rather than per-boot are flags on `ttt_rxbench`, so
the ablation can apply them one at a time: `--cpu`, `--fifo`, `--mlock`,
`--busy-poll-us`, `--prefer-busy-poll`, `--rcvbuf`. The manifest records what
the kernel reports for each.

## 13. AF_XDP, written against the kernel directly

`src/bpf/xdp_redirect.bpf.c` redirects IPv4 UDP to the feed's destination to the
AF_XDP socket on the queue it arrived on, and passes everything else, ARP and
IGMP included. A feed packet whose queue has no socket is passed to the kernel,
not dropped, and counted. That case (the NIC's RSS spread the flow and the socket
is bound elsewhere) is the textbook way AF_XDP loses traffic with no error
anywhere, so it gets its own counter. The program is embedded through a libbpf
skeleton, so each binary carries it.

`src/xdp/xsk.cpp` sets up the socket with raw system calls rather than libxdp's
helpers: register the UMEM, size the fill, completion and RX rings, map them
from `XDP_MMAP_OFFSETS`, bind. The goal is for every step the zero-copy claim
depends on to be in one readable file.

**Modes are forced, then read back.** The program attaches with
`XDP_FLAGS_DRV_MODE` or `XDP_FLAGS_SKB_MODE`, never with neither, and the
attached mode is then read back with `bpf_xdp_query`. The socket binds with
`XDP_ZEROCOPY` or `XDP_COPY`, never with neither, since with neither the kernel
tries zero-copy and falls back to copy without a word. The mode is then read back
with `getsockopt(XDP_OPTIONS)`, and the constructor throws if the two disagree.

**Every frame is always somewhere known.** A frame is either on this process's
free list or with the kernel (fill ring, NIC, RX ring). It comes back only
through the RX ring and returns to the fill ring once its packet is handled.
`free + with_kernel == frames` is asserted on every batch.

**UMEM placement is read from the kernel.** `get_mempolicy` with
`MPOL_F_NODE | MPOL_F_ADDR` gives the node the first page is actually on. Docker's
kernel is built without `CONFIG_NUMA` and cannot say, so the answer there is -1.
The test accepts -1 only where `/sys/devices/system/node` does not exist.

**Checked on a veth pair in the container**, with the sender in its own network
namespace. veth supports native XDP and not zero-copy, which is exactly what
the gate logic needs to be tested against:

- A forced zero-copy bind fails with `EOPNOTSUPP` and the error names the mode.
  It does not become a copy-mode socket.
- A forced native attach is reported as native by the kernel.
- In copy mode, native and generic, every message arrives, the book digest
  matches the oracle, the program's redirect count covers every datagram the
  socket delivered, and the socket reports no drops.
- With no socket bound, the feed is counted as "no socket on queue" and nothing
  else anywhere reports a problem. That is the trap, reproduced on purpose.

`ttt_xdp_probe` is the Gate 0 tool: it runs those checks against a real
interface and prints PASS or FAIL per step. On the veth pair, zero-copy gives
`bind FAIL ... Operation not supported` and `GATE FAIL`, and copy with traffic
passes every check, including a non-zero NAPI id on the socket, which busy
polling needs. `ttt_rxbench --path xdp:IF:Q:native|generic:copy|zerocopy[:busy]`
measures the path. On XDP it opens no UDP socket for the feed, so a packet the
program misses cannot be quietly delivered by the kernel instead. A busy-polled
copy-mode run on veth delivered all 50,004 messages while the kernel's UDP
input counter did not move at all. That is the direct evidence that the feed
bypassed the kernel's UDP stack.

**The trailer now has a size limit.** A 1472-byte packet plus the 32-byte trailer
no longer fits a 1500-byte MTU, and the kernel fragments it. An XDP program sees
the UDP header only in the first fragment. The feed server refuses a budget
above 1440 bytes when the trailer is on.

## 14. The box, scripted before it exists

`tools/box/` is what runs on the Linux machine: `setup_check.sh`, `topology.sh`
(sender port into its own network namespace, cabled to the receiver port),
`gate0_nic.sh`, `knobs.sh` (every set is read back, and a set that does not stick
fails), `ablation.sh` (one knob against stock, both receive modes, two rates) and
`summarize.py`. The order is in `docs/box-runbook.md`. All of it was run on a
veth pair in the container, where it found three bugs that would otherwise have
cost days on the box:

- **A warmup that broke decoding.** `--warmup-ms` skipped stripping the trailer
  as well as recording it, so every warmup datagram reached the decoder 32
  bytes too long and was rejected. The first ablation "measured" a receiver that
  applied nothing. Stripping is now unconditional, and a test runs a warmup and
  requires every message applied.
- **`set -euo pipefail` against `grep`.** A `grep` that matched nothing ended
  the ablation after its first run, silently, since the rest was simply never
  attempted.
- **A gate that can fail for the right reason.** On veth, Gate 0 passed the
  driver, queue, forced native attach and copy bind, and no loss at 20k and 100k
  pps, then failed step 5 because veth has no hardware timestamps, and printed
  `GATE 0 FAIL`. That is the behaviour wanted on hardware that cannot do what the
  project claims.

`knobs.sh restore-stock` was checked by ablating `thp_never` and confirming THP
was back to `always` afterwards.

**The measurement sender does nothing between sends.** With the servers on, the
send thread also fed the rewind and snapshot servers and polled their sockets.
`--preload` builds every packet before the first send, and `--no-servers` removes
the servers. In the container, sender lag at p50 went from 628 ns to 43 ns. The
container's tails are hypervisor scheduling and are not evidence of anything.
End of session now takes its sequence number from the last packet header sent on
feed A, not from the rewind server, which may not be running.

**Zero is a value.** The histogram used to clamp 0 to 1 and count it as clamped.
A sender exactly on schedule at nanosecond resolution records 0, which is a
measurement. Only negative values are clamped now.

## 15. Groundwork for external timing and the jitter hunt

**Kernel receive timestamps.** `--path recvmsg-ts` receives with `recvmsg` and
`SO_TIMESTAMPING`, reads `CLOCK_REALTIME` straight after the call, and records the
software stamp to user space as `stack_to_user`: time in the stack, the socket
queue and the wakeup, all on one clock. `recvmsg-ts:IFACE` first asks the NIC to
stamp every packet (`SIOCSHWTSTAMP` with `HWTSTAMP_FILTER_ALL`), refuses to run if
the driver cannot or substitutes a narrower filter, and adds `nic_to_user`. That
one compares the NIC's clock with the system clock, so it means something only
with `phc2sys` disciplining the system clock, and `phc2sys`'s residual is its
error bar. Datagrams missing a stamp are counted, never silently left out. In the
container, every datagram carried a software stamp, and asking loopback for
hardware stamps failed with "Operation not supported", which is what it should
do. These are internal views. The external measurement, hardware stamps on the
wire on one clock, is still to be built, and its outgoing probe packet waits on
a decision about the scope line.

**The outlier log.** `--outliers-over-ns N` logs every datagram whose schedule to
handled time exceeds N: receive time on the monotonic clock, first sequence
number, and the timings. The log is reserved up front and never grows during a
run, so logging an outlier cannot itself cause one, and overflow is counted.
Rejected: a lock-free ring drained by another core. That moves the same bytes
through more machinery, and the receiver's own core is only a correlation input
here, not something that needs isolating from a writer.

**Joining outliers with the kernel.** `tools/box/jitter_record.sh` runs the
receiver while recording, on its core, IRQ and softirq entry, user page faults,
migrations, context switches, hrtimer expiry, tick stop and workqueue work with
`perf -k CLOCK_MONOTONIC`, the same clock as the outlier log. It also samples
compaction and THP counters and turbostat's SMI count, since SMIs never appear in
a kernel trace. `tools/box/jitter_correlate.py` reports, per event type, the share
of outliers with such an event on the core just before them, and the share with
nothing at all, which is reported rather than dropped. Its self-test caught its
own first bug: the event-name pattern kept a trailing colon, so no event would
ever have matched its type. Sharing a window is not causation. The table picks
what to chase, and a fix with a before-and-after run is the proof.

**NIC receive timestamps on the AF_XDP path.** The BPF object now carries a
second program, `ttt_xdp_redirect_ts`. It calls the driver's
`bpf_xdp_metadata_rx_timestamp` kfunc and writes the stamp, the kfunc's return
code and a magic number as 16 bytes of XDP metadata in front of each redirected
packet. `XdpPath` reads the metadata from the frame's headroom and records
`nic_to_user`, counting each frame as stamped, unstamped (the driver had no
stamp) or metadata missing (it never reached the socket), so every frame is in
exactly one bucket. A kfunc call is accepted by the verifier only in a program
bound to its device, and a device-bound program cannot attach in generic mode,
so asking for timestamps in generic mode is refused, not quietly turned into no
timestamps. Only the program in use is loaded. On the container's veth pair
(Linux 6.12), the device-bound program loads, metadata reaches the socket even
in copy mode (0 missing), and the kfunc succeeds with a zero stamp because veth
has no clock, so every frame counts as unstamped. The plumbing is proven, and the
numbers have to come from the i226.

## 16. Counting copies, not asserting them

`tools/box/paths.sh` runs every receive path under the same conditions and wraps
each receiver in `perf stat`, so the five-way table can say "one fewer copy"
because it was counted. Picking what to count took three tries in the container,
and each wrong answer looked plausible:

- `skb_copy_datagram_iter` fired 5 times for 2,350 datagrams. On Linux 6.12 UDP
  copies a linear skb through `copy_linear_skb`, which goes straight to
  `copy_to_iter`, and `_copy_to_iter` cannot be probed.
- A kprobe left in the kernel by an earlier run made adding it again fail, which
  the first script read as "this kernel has no such function". The summarizer
  then printed the missing counter as 0.000 copies per datagram, which is worse
  than printing nothing. Stale probes are removed first now, and a counter that
  was not collected prints as n/a.
- `skb_consume_udp` runs once for each datagram that leaves a UDP socket after its
  single copy to user memory. It counted exactly 3,514 for 3,514 datagrams on
  recvfrom, recvmmsg, epoll and io_uring.

AF_XDP does its work in softirq context, not in the receiver's process, so its
probes are counted system-wide over each run. `__xsk_rcv` is the copy into the
UMEM in copy mode: 3,514 for 3,514. `__xsk_rcv_zc` also counted 3,514 in copy
mode. Despite its name, it is the enqueue onto the RX ring in both modes. So
zero-copy on the real NIC has to show up as `__xsk_rcv` at zero, and a script
that looked for `__xsk_rcv_zc` being non-zero would have called copy mode
zero-copy. On veth in the container the table reads: every socket path 1.000
copies per datagram (epoll-et at 3.1 system calls per datagram, the others
about 1.1), AF_XDP copy mode 1.000 copies and 1.000 rx ring enqueues.

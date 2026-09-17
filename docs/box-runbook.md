# Box runbook

What to do, in order, when the Linux machine exists. Every step writes its output
under `measurements/`, and that output is what gets committed, never a summary
typed from memory.

## 0. Build

```sh
sudo apt install build-essential clang llvm cmake ninja-build pkg-config zlib1g-dev \
    liburing-dev libbpf-dev libxdp-dev bpftool ethtool tcpdump linux-tools-$(uname -r) \
    numactl linuxptp python3
git submodule update --init
cmake --preset release -B build/linux-release && cmake --build build/linux-release
ctest --test-dir build/linux-release   # the veth AF_XDP tests need sudo
```

## 1. What the machine is

```sh
tools/box/setup_check.sh
sudo tools/box/knobs.sh save-stock      # once, before any tuning, ever
```

`save-stock` records the untuned state. Every ablation run restores to it, so it
must be taken before anything has been changed.

## 2. The rig

Cable the sender port to the receiver port, then:

```sh
sudo SENDER_IF=enp1s0 RX_IF=enp2s0 tools/box/topology.sh up
```

The sender port moves into its own network namespace. Without that, the kernel
routes between two local addresses over loopback and nothing touches the cable.

## 3. Gate 0

```sh
sudo ethtool -L enp2s0 combined 1
sudo SENDER_IF=enp1s0 RX_IF=enp2s0 tools/box/gate0_nic.sh
```

It must print `GATE 0 PASS`. It checks the driver, a single RX queue, forced
native XDP and forced zero-copy both read back from the kernel with live traffic,
no loss by sequence number at every rate in `RATES` with the NIC's and socket's
counters as second witnesses, and hardware receive timestamps with filter `all`.
If step 3 or 4 fails, stop: the AF_XDP half of the project is re-planned before
anything is measured. If only step 5 fails, external timing needs a second
machine.

## 4. Receive paths, all of them, with counters

```sh
export ITCH_FILE=/path/to/01302019.NASDAQ_ITCH50.gz
sudo -E tools/box/paths.sh 5
tools/box/summarize.py measurements/paths --baseline recvfrom > docs/receive-paths.md
```

Every path runs under the same conditions: receiver pinned to `RX_CPU`, sender
pinned in its namespace in measurement mode (`--trailer --busy-wait --preload
--no-servers`), warmup excluded. The second table in the summary is the
explanation: system calls, copies and context switches per datagram. Socket
paths should show one copy per datagram, AF_XDP copy mode one copy into the
UMEM, and AF_XDP zero-copy none. If zero-copy shows copies, it is not zero-copy,
whatever the bind said. A run whose receiver reports `missing` other than 0 is
left out.

Keep the packet rate within the link. At 2.5 Gb/s a 1440-byte payload fits about
200k packets a second, so high-rate runs use small budgets (`BUDGET`, 300 by
default).

## 5. Tuning ablation

```sh
export ITCH_FILE=/path/to/01302019.NASDAQ_ITCH50.gz
for i in $(seq 10); do sudo -E tools/box/ablation.sh baseline 1; done   # the noise floor
for k in fifo mlock irq_affinity cstates governor thp_never thp_always; do
    sudo -E tools/box/ablation.sh $k 5
done
tools/box/summarize.py measurements/ablation > docs/tuning-ablation.md
```

Boot knobs (`isolcpus`, `nohz_full`, `rcu_nocbs`, `cstates_boot`) need the
parameter on the kernel command line and a reboot. `ablation.sh` refuses to run
them unless `/proc/cmdline` really has it. Remove the parameter again before the
next knob, because each knob is measured alone.

## 6. Where the time goes, from the kernel's side

```sh
sudo taskset -c 7 build/linux-release/apps/ttt_rxbench --path recvmsg-ts:enp2s0 \
    --feed 10.77.0.1:30001 --warmup-ms 10000 --latency-out measurements/timestamps/run1
```

`stack_to_user` needs nothing else. `nic_to_user` needs `phc2sys -s enp2s0 -c
CLOCK_REALTIME -O 0 -m` running, and its offset log kept next to the result as
the error bar.

## 7. Jitter hunt

```sh
sudo tools/box/jitter_record.sh measurements/jitter/before -- --path recvmmsg:32 \
    --feed 10.77.0.1:30001 --warmup-ms 10000 --outliers-over-ns 200000 \
    --latency-out measurements/jitter/before/rx
tools/box/jitter_correlate.py measurements/jitter/before/rx-outliers.csv \
    measurements/jitter/before/perf.txt --cpu 7
```

Pick the cause at the top of the table, fix it, and repeat the same run into
`measurements/jitter/after`. The before and after distributions are the result.

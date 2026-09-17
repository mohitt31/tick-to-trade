# Linux build and functional-test environment for the Linux-only receive paths.
#
# On the Mac this runs in Docker's arm64 Linux VM. It checks that the Linux code
# builds and behaves correctly. It is never a place to measure: the VM's network
# is virtual and nothing in it is isolated. Numbers come from bare metal only.
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang llvm lld cmake ninja-build git pkg-config ca-certificates \
        zlib1g-dev liburing-dev libbpf-dev libxdp-dev xdp-tools bpftool \
        linux-libc-dev iproute2 ethtool tcpdump procps iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Tools the box scripts need, in their own layer so the one above stays cached.
RUN apt-get update && apt-get install -y --no-install-recommends python3 linux-perf \
    && rm -rf /var/lib/apt/lists/*

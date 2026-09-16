#!/bin/sh
# Runs a command in the Linux dev container, with this repo mounted at /src.
#
#   tools/docker_dev.sh cmake --preset linux-release
#   tools/docker_dev.sh            # a shell
#
# --privileged is needed for XDP and network namespaces, and the default seccomp
# profile blocks io_uring, so it is lifted. Both are for functional tests of the
# Linux code only; nothing measured in this container is ever a result.
set -eu
cd "$(dirname "$0")/.."
image=ttt-dev
docker build -q -t "$image" -f docker/dev.Dockerfile docker > /dev/null
if [ $# -eq 0 ]; then
    set -- bash
fi
tty=""
if [ -t 0 ]; then
    tty="-t"
fi
exec docker run --rm -i $tty --privileged --security-opt seccomp=unconfined \
    -v "$PWD":/src -w /src "$image" "$@"

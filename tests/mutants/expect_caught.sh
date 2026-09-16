#!/bin/sh
# Runs the chaos harness with one planted bug and passes only if the harness
# ran and reported a failing seed. A plain `! command` would also pass when the
# binary is missing or crashes before running anything.
bin=$1
mutant=$2
out=$("$bin" --mutant "$mutant" --seeds 2000 2>&1)
status=$?
if [ "$status" -eq 0 ]; then
    echo "mutant $mutant survived 2000 seeds"
    exit 1
fi
if ! printf '%s\n' "$out" | grep -Eq '(^seed [0-9]+: FAILED|assertion failed on seed [0-9]+)'; then
    echo "harness exited $status without reporting a failing seed:"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
printf '%s\n' "$out" | grep -E '(^seed [0-9]+: FAILED|assertion failed on seed [0-9]+)' | head -1

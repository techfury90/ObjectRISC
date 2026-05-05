#!/bin/sh
# test_concurrent.sh — end-to-end test of the multi-child task table.
#
# Builds and runs examples/cc/multitask/concurrent.c — bootstrap
# spawns five children all at once (each with a different stamp
# value), waits on each in turn, and prints what they wrote into a
# shared scratch object. Asserts on the exact stdout sequence.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

ACTUAL=$(bash examples/cc/multitask/run-concurrent.sh 2>&1)
EXPECTED='spawned 5 children
scratch[0] = 7
scratch[1] = 11
scratch[2] = 13
scratch[3] = 17
scratch[4] = 19
all done'

if [ "$ACTUAL" = "$EXPECTED" ]; then
    echo "PASS"
    exit 0
fi

echo "FAIL: stdout mismatch" >&2
echo "--- expected ---" >&2
printf '%s\n' "$EXPECTED" >&2
echo "--- got ---" >&2
printf '%s\n' "$ACTUAL" >&2
exit 1

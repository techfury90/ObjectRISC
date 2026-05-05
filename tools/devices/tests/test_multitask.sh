#!/bin/sh
# test_multitask.sh — end-to-end test of the task.c libc layer.
#
# Builds and runs examples/cc/multitask/multitask.c — bootstrap
# spawns three child tasks in sequence, each doubles its R4 arg
# and exits with that as its exit code, parent prints the result.
# Asserts on the exact stdout sequence.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

ACTUAL=$(bash examples/cc/multitask/run.sh 2>&1)
EXPECTED='child(7) -> 14
child(11) -> 22
child(21) -> 42
parent done'

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

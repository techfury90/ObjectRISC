#!/bin/sh
# test_fork_reap_loop.sh — stress the task_spawn -> task_wait -> task_free
# reap path. Compiles examples/cc/fork_reap_loop.c and runs it under
# simorisc; the program spawns, waits, and frees a child 24 times reusing
# the same table slot, and returns 42 iff every iteration succeeds and the
# parent's global is untouched by any child (fork isolation across reuse).
#
# Regression guard for free-on-reap: task_free now reclaims each child's
# stack AND private data copy (task_store_res / task_free_res). A bug there
# — freeing the wrong ref, a live-object double-free, or corrupting the
# resource slots — surfaces as a failed spawn/wait/free or a trap within a
# few iterations. (Measured out-of-band: with the reap the sim frees 2 extra
# objects per iteration — the stack + data copy — that otherwise leak to CPU
# teardown.) Self-contained: one CPU, no external services.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    echo "SKIP: pcc not built at $PCC_BUILD (run tools/cc/build.sh)" >&2
    exit 0
fi

rc=0
examples/cc/run_c.sh examples/cc/fork_reap_loop.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: fork_reap_loop.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

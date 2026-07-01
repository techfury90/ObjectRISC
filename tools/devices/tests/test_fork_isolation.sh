#!/bin/sh
# test_fork_isolation.sh — proves task_spawn gives a child a PRIVATE data
# segment (fork semantics). Compiles examples/cc/fork_isolation.c and runs it
# under simorisc; the program returns 42 iff a task_spawn'd child (a) boots
# from a SNAPSHOT of the parent's live globals (parent sets `shared = 55`
# before spawn; child must see 55, not the initializer 100 or zero) and (b)
# writes to its own COPY (`shared = 999`) without the parent's copy changing
# (parent still reads 55 afterward). A smaller code names the failed check
# (parent 2/3/9, child 20/21; see fork_isolation.c).
#
# Regression guard for the fork model: before it, task_spawn left the child
# mapping the parent's OWN data object, so parent+child shared all C globals —
# the child's `shared = 999` would reach the parent (this test would return 9),
# and a child's task_init clobbered the parent's per-task libc state. Now the
# child gets a byte-copy of the parent's data at spawn (ObjFetchBytes #0x108).
# Self-contained: two tasks on one CPU, no external services.

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
examples/cc/run_c.sh examples/cc/fork_isolation.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: fork_isolation.c returned $rc, expected 42 (9 = writes bled through; child 20 = snapshot lost)" >&2
    exit 1
fi

echo "PASS"

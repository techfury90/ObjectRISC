#!/bin/sh
# test_objor_console.sh — object-console result-sink PoC for the `void *__or`
# value API. Compiles examples/cc/objor_console.c and runs it under simorisc;
# the program returns 42 iff a "shell" task can stand up a receive-queue sink,
# task_spawn a "command" task that streams N typed result objects to the sink
# (objor_send_cap with a per-result kind) plus an END marker, and then collect
# each result IN ORDER, inspect it AS AN OBJECT (objor_loadw), and task_wait
# the command's exit code. A smaller code marks the first failed check
# (collect-side 2..9/30+, produce-side 12..13; see objor_console.c).
#
# This is the degenerate text projection of the object-console model: a command
# emits typed result OREFs to a Session-owned sink and exits; the shell collects
# them. It exercises objor_queue_attach / objor_send_cap / objor_send /
# objor_recv_cap plus objor_stash_o7 / objor_adopt_o7, and — because the shell
# task_waits the command AFTER the command's task_init has run — it is the
# regression guard for the per-task task-table fix (a task_spawn'd child's
# task_init must not clobber the parent's task_slots bookkeeping). The shell
# exits LAST here, so its 42 genuinely depends on task_wait working (it is not
# masked by the child's exit code).
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
examples/cc/run_c.sh examples/cc/objor_console.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: objor_console.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

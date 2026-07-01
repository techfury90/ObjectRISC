#!/bin/sh
# test_objor_grant.sh — cross-task capability-GRANT proof for the `void *__or`
# messaging API (tools/cc/lib/obj_or.{h,s}). Compiles examples/cc/objor_grant.c
# and runs it under simorisc; the program returns 42 iff a parent task can
# publish a rendezvous mailbox to a task_spawn'd child, the child can announce
# its reply address via objor_send_cap, and the parent can SEND the child a
# derived READ-ONLY capability that the child RECEIVEs (objor_recv_cap) and
# uses under the narrowed rights (a smaller code says which check failed:
# parent-side 2..7, child-side 12..18). This exercises the messaging half of
# the value API (objor_queue_attach / objor_send_cap / objor_recv_cap) plus
# the O-register<->value bridge (objor_stash_o7 / objor_adopt_o7) — the
# "a pipe is a capability grant" pattern the object-console shell is built on.
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
examples/cc/run_c.sh examples/cc/objor_grant.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: objor_grant.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

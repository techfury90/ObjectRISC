#!/bin/sh
# test_obj_or_smoke.sh — end-to-end test of the `void *__or` capability-
# VALUE object API (tools/cc/lib/obj_or.{h,s}). Compiles
# examples/cc/obj_or_smoke.c and runs it under simorisc; the program
# returns 42 iff allocate / marker round-trip / inspect / derive / read-
# through-sub / drop / free / reuse all behave with capabilities held as C
# `void *__or` values (a smaller code says which step failed). No services
# or peer tasks are needed — it's self-contained. The value-world sibling
# of test_obj_smoke.sh.

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
examples/cc/run_c.sh examples/cc/obj_or_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: obj_or_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

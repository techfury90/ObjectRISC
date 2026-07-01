#!/bin/sh
# test_objor_delegate.sh — capability-DELEGATION proof for the `void *__or`
# capability-value object API (tools/cc/lib/obj_or.{h,s}). Compiles
# examples/cc/objor_delegate.c and runs it under simorisc; the program
# returns 42 iff a resource can be minted, a RESTRICTED (read-only)
# capability derived and handed to a helper AS A `__or` VALUE PARAM, and
# used across call boundaries under the narrowed rights (a smaller code says
# which check failed). This is the pattern the document architecture and
# object-console shell are built on — the value-passing case obj_or_smoke.c
# does not exercise. Self-contained: no services or peer tasks.

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
examples/cc/run_c.sh examples/cc/objor_delegate.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: objor_delegate.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

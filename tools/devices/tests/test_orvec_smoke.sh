#!/bin/sh
# test_orvec_smoke.sh — end-to-end test of the register-indexed OREFLD/OREFST
# addressing mode (orefldx/orefstx) via the objor_vget/objor_vset wrappers.
# Compiles examples/cc/orvec_smoke.c and runs it under simorisc; the program
# returns 42 iff an OR-typed capability array round-trips through
# RUNTIME-indexed store/load (a smaller code says which step failed). No
# services or peer tasks — self-contained, like test_obj_or_smoke.sh.

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
examples/cc/run_c.sh examples/cc/orvec_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: orvec_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

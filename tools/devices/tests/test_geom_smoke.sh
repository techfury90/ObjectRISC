#!/bin/sh
# test_geom_smoke.sh — exercises the plain-C geometry ops (tools/cc/lib/geom).
# Compiles examples/cc/geom_smoke.c and runs it under simorisc; returns 42 iff
# rect_empty / contains / eq / intersect / union all behave. Self-contained,
# no objects or services.

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
examples/cc/run_c.sh examples/cc/geom_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: geom_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

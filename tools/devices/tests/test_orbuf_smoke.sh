#!/bin/sh
# test_orbuf_smoke.sh — proves the growable byte buffer orbuf
# (tools/cc/lib/orbuf). Compiles examples/cc/orbuf_smoke.c and runs it under
# simorisc; returns 42 iff N byte-spans append through a growing buffer
# (8->16->32->64), sources can be freed immediately (bytes copied), and each
# span reads back correctly in order. Self-contained.

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
examples/cc/run_c.sh examples/cc/orbuf_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: orbuf_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

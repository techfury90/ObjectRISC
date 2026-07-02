#!/bin/sh
# test_orvec_section.sh — proves the growable orvec (tools/cc/lib/orvec)
# against the document Section->Blocks shape. Compiles
# examples/cc/orvec_section.c and runs it under simorisc; returns 42 iff a
# section's block array grows (2->4->8->16), every block survives the
# reallocs in order, and a block can be frozen (freed + slot nulled) and
# thawed with neighbours + length intact. Self-contained.

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
examples/cc/run_c.sh examples/cc/orvec_section.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: orvec_section.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

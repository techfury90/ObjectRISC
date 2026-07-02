#!/bin/sh
# test_doc_freeze.sh — proves the document model (tools/cc/lib/doc) and the
# freeze-on-scroll-away lifecycle. Compiles examples/cc/doc_freeze.c and runs
# it under simorisc; returns 42 iff a Document of N blocks builds (growing),
# reads back correctly, freezes its first K blocks (live object count drops to
# N-K, bytes preserved in the text-log), thaws them by walking the log, and
# still reads back correctly. Self-contained.

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
examples/cc/run_c.sh examples/cc/doc_freeze.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: doc_freeze.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

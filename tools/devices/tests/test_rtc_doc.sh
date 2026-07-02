#!/bin/sh
# test_rtc_doc.sh — the doc producer + layout on REAL text (tools/cc/lib/doc
# block_from_mem + tools/cc/lib/rtc rtc_layout). Compiles
# examples/cc/rtc_doc_smoke.c and runs it under simorisc; returns 42 iff a
# Document built from C strings (a heading + a wrapping paragraph + a short
# paragraph) lays out into the expected display-list lines (source block, byte
# spans, stacked y). Exercises producer -> doc -> __or->handle bridge -> fetch
# -> wrap. Self-contained (no WM).

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
examples/cc/run_c.sh examples/cc/rtc_doc_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: rtc_doc_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

#!/bin/sh
# test_rtc_layout.sh — exercises rtc_layout's object path (tools/cc/lib/rtc):
# build a Document, bridge each __or block ref to a handle, lay the handles
# out, and check per-block placement (index, kind, text length, stacked y) and
# total height. Compiles examples/cc/rtc_layout_smoke.c and runs it under
# simorisc; returns 42 iff the walk + fetch + header-parse + emit all behave.
# Self-contained (no WM). Wrap correctness is covered by test_rtc_wrap.sh.

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
examples/cc/run_c.sh examples/cc/rtc_layout_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: rtc_layout_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

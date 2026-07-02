#!/bin/sh
# test_rtc_wrap.sh — exercises the Rich Text Control's word-wrap algorithm
# (tools/cc/lib/rtc: rtc_wrap_text). Compiles examples/cc/rtc_wrap_smoke.c and
# runs it under simorisc; returns 42 iff word wrap, over-long-word hard breaks,
# explicit newlines, and the empty case all produce the expected display-list
# lines. Self-contained (no objects, no WM).

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
examples/cc/run_c.sh examples/cc/rtc_wrap_smoke.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: rtc_wrap_smoke.c returned $rc, expected 42" >&2
    exit 1
fi

echo "PASS"

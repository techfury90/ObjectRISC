#!/bin/sh
# test_termfw_splash.sh — terminal firmware splash (M1), standalone.
#
# Builds termfw.orx (short self-test delay so CI is fast) and runs it under
# simorisc with no peers: it allocates the framebuffer, runs the VRAM self-test
# with pixel read-back, and prints the Lucida Typewriter banner.  A second build
# forces a memtest failure to confirm the fail path renders its message and
# exits nonzero.  Headless (no --display); the visual splash is eyeballed via
# `simorisc --display tk build/termfw.orx`.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/console_io.oro"

# build_termfw <extra-cppflags> <out.orx>
build_termfw() {
    "$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
        -I tools/cc/arch/orisc -I tools/cc/lib $1 ouroboros/termfw.c > "$TMP/t.i"
    "$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/t.i" > "$TMP/t.s"
    python3 tools/asm/asmorisc -r "$TMP/t.s" -o "$TMP/t.oro"
    python3 tools/ld/orld -o "$2" \
        "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/t.oro" build/liborisc.ora
}

# --- 1. normal: self-test passes, banner draws, clean exit 0 ---
build_termfw "-DDELAY_US=2000 -DBANNER_HOLD_US=2000" "$TMP/termfw.orx"
rc=0
python3 tools/sim/simorisc "$TMP/termfw.orx" > "$TMP/ok.out" 2>&1 || rc=$?
echo "--- termfw (pass), exit=$rc ---"; cat "$TMP/ok.out"
[ "$rc" -eq 0 ] || { echo "FAIL: pass build exited $rc (expected 0)" >&2; exit 1; }
grep -q "termfw: self-test PASS" "$TMP/ok.out" \
    || { echo "FAIL: never reached PASS line" >&2; exit 1; }
if grep -q "FAIL:" "$TMP/ok.out"; then
    echo "FAIL: termfw reported FAIL on the pass path" >&2; exit 1
fi

# --- 2. forced memtest failure: message renders + nonzero exit ---
build_termfw "-DDELAY_US=2000 -DBANNER_HOLD_US=2000 -DFORCE_MEMTEST_FAIL" "$TMP/fail.orx"
rc=0
python3 tools/sim/simorisc "$TMP/fail.orx" > "$TMP/fail.out" 2>&1 || rc=$?
echo "--- termfw (forced memtest fail), exit=$rc ---"; cat "$TMP/fail.out"
[ "$rc" -ne 0 ] || { echo "FAIL: forced-memtest build exited 0 (expected nonzero)" >&2; exit 1; }
grep -q "FAIL: memtest" "$TMP/fail.out" \
    || { echo "FAIL: forced-memtest didn't print 'FAIL: memtest'" >&2; exit 1; }

echo "PASS"

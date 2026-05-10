#!/bin/sh
# test_vec_smoke.sh — end-to-end test of WM-mediated vector graphics
# (Phase 59 / WM γ.11).
#
# Mirrors test_wm_smoke.sh exactly; the only difference is the smoke
# program (vec_smoke.c instead of wm_smoke.c) and the success line
# (`vec_smoke: PASS` vs `wm_smoke: PASS`).
#
# Architecture under test (same daemons as test_wm_smoke):
#   - oriscbar (the crossbar)
#   - oriscdir at pid 18
#   - oriscterm (fake) at pid 16
#   - simorisc CPU 0 running oriscwm.orx
#   - simorisc CPU 1 running vec_smoke.orx (the test client)
#
# vec_smoke exercises every VEC_OP_* through the libc vec_*() helpers
# and verifies each returns 0.  Pixel accuracy is not asserted —
# eyeball check on `make boot` covers that.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# --- build the WM (.orx) -------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib ouroboros/oriscwm.c \
    > "$TMP/wm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/wm.i" > "$TMP/wm.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/wm.s"                       -o "$TMP/wm.oro"
python3 tools/ld/orld -o "$TMP/oriscwm.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/wm.oro" \
    build/liborisc.ora

# --- build vec_smoke.orx -------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/vec_smoke.c \
    > "$TMP/sm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/sm.i" > "$TMP/sm.s"
python3 tools/asm/asmorisc -r "$TMP/sm.s"                       -o "$TMP/sm.oro"
python3 tools/ld/orld -o "$TMP/vec_smoke.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/sm.oro" \
    build/liborisc.ora

# --- launch oriscbar -----------------------------------------------
SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# --- launch oriscdir at pid 18 -------------------------------------
python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 -v \
    > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do
    grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch oriscterm at pid 16 (publishes surfaces) ---------------
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    --linger 8.0 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch the WM CPU at pid 0 ------------------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" \
    "$TMP/oriscwm.orx" >"$TMP/wm.out" 2>"$TMP/wm.err" &
WM_CPU=$!

for _ in $(seq 100); do
    grep -q "oriscwm: ready" "$TMP/wm.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch the smoke-test CPU at pid 1 ----------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" \
    "$TMP/vec_smoke.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $CPU 2>/dev/null || true
CPU_RC=$?

kill -TERM $WM_CPU   2>/dev/null || true
kill -TERM $TERM_PID 2>/dev/null || true
kill -TERM $DIR      2>/dev/null || true
kill -TERM $BAR      2>/dev/null || true
wait $WM_CPU 2>/dev/null || true
wait $TERM_PID 2>/dev/null || true
wait $DIR      2>/dev/null || true
wait $BAR      2>/dev/null || true

echo "--- vec_smoke (cpu1) stdout ---"
cat "$TMP/cpu.out"
echo "--- vec_smoke (cpu1) stderr ---"
cat "$TMP/cpu.err"
echo "--- oriscwm (cpu0) stdout ---"
cat "$TMP/wm.out"
echo "--- oriscwm (cpu0) stderr ---"
cat "$TMP/wm.err"

grep -q "oriscwm: ready" "$TMP/wm.out" \
    || { echo "FAIL: WM never reached ready" >&2; exit 1; }

grep -q "vec_smoke: PASS" "$TMP/cpu.out" \
    || { echo "FAIL: smoke-test PASS line missing" >&2; exit 1; }

if grep -q "FAIL:" "$TMP/cpu.out"; then
    echo "FAIL: smoke test reported FAIL" >&2
    exit 1
fi

echo "PASS"

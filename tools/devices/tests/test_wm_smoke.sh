#!/bin/sh
# test_wm_smoke.sh — end-to-end test of the milestone-2 .orx WM.
#
# Architecture under test:
#   - oriscbar (the crossbar)
#   - oriscdir at pid 18 (so the WM can dir_walk for surface caps,
#     and so wm_smoke can bootstrap dir.c if needed)
#   - oriscterm at pid 16 (publishes /sys/term/0/{console,keyboard,grid})
#   - simorisc CPU 0 running oriscwm.orx (the WM itself)
#   - simorisc CPU 1 running wm_smoke.orx (the test client)
#
# wm_smoke exercises every milestone-2 wire op (NEW_WINDOW,
# BIND_SURFACE, DESTROY_WINDOW + the failure paths E_INVAL /
# E_NOSPC / E_NOTIMPL) and prints "wm_smoke: PASS" on success.
#
# This replaces the milestone-1 Python-daemon test.  The Python
# daemon at tools/devices/oriscwm has been removed; the wire shape
# changed too (single service + payload-dispatch — see oriscwm.c
# for the protocol writeup).

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

# Build everything once if we don't have liborisc / supervisor — the
# WM and smoke test share the same build pipeline.
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

# --- build wm_smoke.orx --------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/wm_smoke.c \
    > "$TMP/sm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/sm.i" > "$TMP/sm.s"
python3 tools/asm/asmorisc -r "$TMP/sm.s"                       -o "$TMP/sm.oro"
python3 tools/ld/orld -o "$TMP/wm_smoke.orx" \
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
# Boot ABI for the WM: O8 = oriscdir cap (the rest of the boot
# OPRs aren't used — the WM walks oriscdir for surface caps).
# --service slot order: O5/O6/O7 unused, O8 = 18=1@9 (oriscdir).
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" \
    "$TMP/oriscwm.orx" >"$TMP/wm.out" 2>"$TMP/wm.err" &
WM_CPU=$!

# Wait for the WM to register at /sys/wm/0.  We see the "registered"
# banner in its host stdout.
for _ in $(seq 100); do
    grep -q "oriscwm: ready" "$TMP/wm.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch the smoke-test CPU at pid 1 ----------------------------
# Boot ABI for the smoke test: O8 = oriscdir mailbox sub-cap.
# The smoke test uses libc's wm_init() which dir_walks /sys/wm/0
# to discover the WM — no longer brittle to changes in the WM's
# startup-time allocation order (milestone 2 wired the WM mailbox
# idx directly via --service, which broke whenever a new alloc
# slipped in before allocate_service_mailbox).
#
# --service slot order: O5/O6/O7 unused, O8 = 18=1@9 (oriscdir).
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" \
    "$TMP/wm_smoke.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $CPU 2>/dev/null || true
CPU_RC=$?

# Cleanup.
kill -TERM $WM_CPU   2>/dev/null || true
kill -TERM $TERM_PID 2>/dev/null || true
kill -TERM $DIR      2>/dev/null || true
kill -TERM $BAR      2>/dev/null || true
wait $WM_CPU 2>/dev/null || true
wait $TERM_PID 2>/dev/null || { echo "FAIL: fake_terminal aborted (boot/input never came up - see term.out and cpu*.out)" >&2; kill -KILL $(jobs -p) 2>/dev/null; exit 1; }
wait $DIR      2>/dev/null || true
wait $BAR      2>/dev/null || true

echo "--- wm_smoke (cpu1) stdout ---"
cat "$TMP/cpu.out"
echo "--- wm_smoke (cpu1) stderr ---"
cat "$TMP/cpu.err"
echo "--- oriscwm (cpu0) stdout ---"
cat "$TMP/wm.out"
echo "--- oriscwm (cpu0) stderr ---"
cat "$TMP/wm.err"
echo "--- oriscdir log ---"
cat "$TMP/dir.out"

# Assertions.
grep -q "oriscwm: ready" "$TMP/wm.out" \
    || { echo "FAIL: WM never reached ready" >&2; exit 1; }

grep -q "wm_smoke: PASS" "$TMP/cpu.out" \
    || { echo "FAIL: smoke-test PASS line missing" >&2; exit 1; }

if grep -q "FAIL:" "$TMP/cpu.out"; then
    echo "FAIL: smoke test reported FAIL" >&2
    exit 1
fi

echo "PASS"

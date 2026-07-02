#!/bin/sh
# test_ptr_smoke.sh — end-to-end test of WM-mediated pointer events
# (Phase 59 / WM γ.13).
#
# Setup mirrors test_vec_smoke / test_raster_smoke.  The interesting
# bit is event timing: fake_terminal blocks until SOMEONE subscribes
# to its pointer service (= the WM, at boot), then starts firing
# events with --delay between each.  By that point smoke may not
# have subscribed to the WM yet, so the WM defers polling its
# events mailbox until a subscriber exists — the underlying
# ReceiveQueue (depth 64) buffers events for us.
#
# We inject 5 events; smoke needs at least 3 to PASS, which gives
# slack against the buffer-flush ordering.

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

# --- build ptr_smoke.orx -------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/ptr_smoke.c \
    > "$TMP/sm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/sm.i" > "$TMP/sm.s"
python3 tools/asm/asmorisc -r "$TMP/sm.s"                       -o "$TMP/sm.oro"
python3 tools/ld/orld -o "$TMP/ptr_smoke.orx" \
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

# --- launch the WM CPU at pid 0 (owns the pointer sink locally) ----
# Post-Phase-60 the WM owns its pointer sink (TAG_INPUT_SINK via
# #0x10B); nothing is published under /sys/term/0.  init-r4=1 →
# terminal index 0 (matches scripts/boot.sh).
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" --init-r4 1 \
    "$TMP/oriscwm.orx" >"$TMP/wm.out" 2>"$TMP/wm.err" &
WM_CPU=$!

for _ in $(seq 200); do
    grep -q "oriscwm: ready" "$TMP/wm.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch the smoke-test CPU at pid 1 ----------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" \
    "$TMP/ptr_smoke.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

# Wait until ptr_smoke has subscribed to the WM pointer surface, then
# inject the pointer event sequence into the WM CPU's pointer sink
# (--inject-cpu 0) via PKT_HOST_INPUT — byte-identical to live mouse
# input from simorisc's Tk display worker.  Inject only after the
# subscribe so the WM has a subscriber to forward the events to.
for _ in $(seq 300); do
    grep -q "ptr_smoke: subscribed" "$TMP/cpu.out" 2>/dev/null && break
    kill -0 $CPU 2>/dev/null || break
    sleep 0.1
done
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 --inject-cpu 0 \
    --delay 0.3 --linger 2.0 \
    --event motion:100,150 \
    --event down:100,150,1 \
    --event motion:120,170 \
    --event up:120,170,1 \
    --event motion:200,200 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!

wait $CPU 2>/dev/null || true
CPU_RC=$?

kill -TERM $WM_CPU   2>/dev/null || true
kill -TERM $TERM_PID 2>/dev/null || true
kill -TERM $DIR      2>/dev/null || true
kill -TERM $BAR      2>/dev/null || true
wait $WM_CPU 2>/dev/null || true
wait $TERM_PID 2>/dev/null || { echo "FAIL: fake_terminal aborted (boot/input never came up - see term.out and cpu*.out)" >&2; kill -KILL $(jobs -p) 2>/dev/null; exit 1; }
wait $DIR      2>/dev/null || true
wait $BAR      2>/dev/null || true

echo "--- ptr_smoke (cpu1) stdout ---"
cat "$TMP/cpu.out"
echo "--- ptr_smoke (cpu1) stderr ---"
cat "$TMP/cpu.err"
echo "--- oriscwm (cpu0) stdout ---"
cat "$TMP/wm.out"
echo "--- oriscwm (cpu0) stderr ---"
cat "$TMP/wm.err"
echo "--- fake_terminal log ---"
cat "$TMP/term.out"

grep -q "oriscwm: ready" "$TMP/wm.out" \
    || { echo "FAIL: WM never reached ready" >&2; exit 1; }

grep -q "oriscwm: pointer mediation ready" "$TMP/wm.out" \
    || { echo "FAIL: WM pointer mediation never came up" >&2; exit 1; }

grep -q "ptr_smoke: PASS" "$TMP/cpu.out" \
    || { echo "FAIL: smoke-test PASS line missing" >&2; exit 1; }

if grep -q "FAIL:" "$TMP/cpu.out"; then
    echo "FAIL: smoke test reported FAIL" >&2
    exit 1
fi

echo "PASS"

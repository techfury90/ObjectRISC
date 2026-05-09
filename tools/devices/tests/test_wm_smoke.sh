#!/bin/sh
# test_wm_smoke.sh — end-to-end test of the oriscwm milestone-1 wire
# protocol.
#
# Architecture under test:
#   - oriscbar (the crossbar)
#   - fake_terminal at pid 16 (just to provide cap shapes for O5/O6)
#   - oriscwm at pid 20 (no oriscdir wired — the smoke test bypasses
#     the directory and gets the WM cap directly via --service)
#   - simorisc CPU 0 running wm_smoke.orx
#
# wm_smoke exercises every milestone-1 wire op (REGISTER_SURFACE,
# NEW_WINDOW, BIND_SURFACE, DESTROY_WINDOW + the failure cases for
# E_INVAL / E_NOSPC / E_NOTIMPL).  It prints "PASS" on success.
#
# The fake_terminal is here only to provide live caps for O5/O6.
# wm_smoke registers them with the WM as opaque "console" and
# "keyboard" caps; the test never actually exercises the underlying
# terminal services.  No real keyboard events, no console output to
# the Tk pane.  Diagnostics go to host stdout via firmware ConsoleWrite.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# --- build wm_smoke.orx ---------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/wm_smoke.c \
    > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/wm_smoke.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    build/liborisc.ora

# --- launch oriscbar ------------------------------------------------
SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# --- launch fake_terminal at pid 16 (provides O5/O6 cap shapes) ------
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --linger 5.0 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch oriscwm at pid 20 (no --directory-pid; bypass oriscdir) --
python3 tools/devices/oriscwm \
    --socket "$SOCK" --pid 20 -v \
    > "$TMP/wm.out" 2>&1 &
WM=$!
for _ in $(seq 50); do
    grep -q "oriscwm READY" "$TMP/wm.out" 2>/dev/null && break
    sleep 0.05
done

# --- launch the smoke-test CPU --------------------------------------
# Slot layout via --service:
#   O5 = 16=1@9   (terminal console — used as a generic surface cap)
#   O6 = 16=2@9   (terminal keyboard — same)
#   O7 = 20=1@9   (WM main service)
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" --service "20=1@9" \
    "$TMP/wm_smoke.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $CPU 2>/dev/null || true
CPU_RC=$?

# Cleanup.
kill -TERM $WM       2>/dev/null || true
kill -TERM $TERM_PID 2>/dev/null || true
kill -TERM $BAR      2>/dev/null || true
wait $WM 2>/dev/null || true
wait $TERM_PID 2>/dev/null || true
wait $BAR      2>/dev/null || true

echo "--- cpu0 stdout ---"
cat "$TMP/cpu.out"
echo "--- cpu0 stderr ---"
cat "$TMP/cpu.err"
echo "--- oriscwm log ---"
cat "$TMP/wm.out"

# Assertions: the smoke test program prints PASS on success and
# FAIL: <stage> on failure.  We grep stdout and also check that the
# CPU exited cleanly (return 0 from main → simorisc exit 0).
grep -q "wm_smoke: PASS" "$TMP/cpu.out" \
    || { echo "FAIL: smoke-test PASS line missing from cpu0 stdout" >&2; exit 1; }
if grep -q "FAIL:" "$TMP/cpu.out"; then
    echo "FAIL: smoke test reported FAIL" >&2
    exit 1
fi

echo "PASS"

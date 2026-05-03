#!/bin/sh
# test_kbd_echo.sh — end-to-end test of the keyboard service flow.
#
# Builds kbd_echo.orx, launches oriscbar + a fake terminal (no Tk) +
# the demo CPU. The fake terminal sends a sequence of synthetic
# keystrokes via the wire protocol; we capture cpu0's stdout and
# verify each key shows up. Last keystroke is ESC, so the demo
# exits cleanly.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/kbd_echo.c \
    > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/kbd_echo.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    tools/cc/lib/liborisc.ora

SOCK="$TMP/oriscbar.sock"

# 1) Crossbar.
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# 2) Fake terminal first (so pid 16 is connected before the CPU
# fires its subscribe SEND — the crossbar drops packets aimed at
# unknown pids, and kbd_echo has no retry loop).
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:A --event key:B --event key:0x11B \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
# Wait for fake_terminal's READY line.
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# 3) Demo CPU.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    "$TMP/kbd_echo.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

# Wait for the demo CPU to exit (it should, on ESC).
wait $CPU 2>/dev/null || true
wait $TERM_PID 2>/dev/null || true
kill -TERM $BAR 2>/dev/null || true
wait $BAR 2>/dev/null || true

echo "--- cpu0 stdout ---"
cat "$TMP/cpu.out"
echo "--- fake terminal log ---"
cat "$TMP/term.out"

# Assertions: each key should produce a "key=N 'x'" line.
grep -q "key=65 'A'"      "$TMP/cpu.out" \
    || { echo "FAIL: missing 'A' echo" >&2; exit 1; }
grep -q "key=66 'B'"      "$TMP/cpu.out" \
    || { echo "FAIL: missing 'B' echo" >&2; exit 1; }
grep -q "ESC — exiting"   "$TMP/cpu.out" \
    || { echo "FAIL: missing ESC exit message" >&2; exit 1; }

echo "PASS"

#!/bin/sh
# test_mouse_paint.sh — end-to-end test of the pointer service flow.
#
# Builds mouse_paint.orx, launches oriscbar + a fake terminal +
# the demo CPU. The fake terminal sends a sequence of mouse events
# (left-click + drag + release, then middle-click for color cycle,
# then right-click for clear). We verify cpu0's stdout shows the
# expected log lines.
#
# mouse_paint runs forever (no clean exit signal), so we SIGTERM
# the CPU after the events have been delivered.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/mouse_paint.c \
    > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/mouse_paint.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    build/liborisc.ora

SOCK="$TMP/oriscbar.sock"

# 1) Crossbar.
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# 2) Fake terminal with the test sequence.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event down:100,150,1 \
    --event motion:120,170 \
    --event up:120,170,1 \
    --event down:0,0,2 \
    --event up:0,0,2 \
    --event down:0,0,3 \
    --event up:0,0,3 \
    --linger 0.5 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# 3) Demo CPU. Slot order matches run_mouse_paint.sh.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" --service "16=3@9" \
    --service "16=4@9" --service "16=6@9" \
    "$TMP/mouse_paint.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

# Wait for fake_terminal to finish its event sequence.
wait $TERM_PID 2>/dev/null || true
# Brief grace so the demo finishes processing the last event.
sleep 0.5
# mouse_paint runs forever — SIGTERM it now that the events are delivered.
kill -TERM $CPU 2>/dev/null || true
wait $CPU 2>/dev/null || true
kill -TERM $BAR 2>/dev/null || true
wait $BAR 2>/dev/null || true

echo "--- cpu0 stdout ---"
cat "$TMP/cpu.out"
echo "--- fake terminal log ---"
cat "$TMP/term.out"

# Assertions.
grep -q "mouse_paint ready"  "$TMP/cpu.out" \
    || { echo "FAIL: missing ready banner" >&2; exit 1; }
grep -q "color cycled to 2"  "$TMP/cpu.out" \
    || { echo "FAIL: middle-click color-cycle did not register" >&2; exit 1; }
grep -q "canvas cleared"     "$TMP/cpu.out" \
    || { echo "FAIL: right-click clear did not register" >&2; exit 1; }

echo "PASS"

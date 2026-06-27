#!/bin/sh
# test_wm_scrollbar.sh — scrollbar arrow AUTO-REPEAT (Stage B/C interaction).
#
# Boots the WM with -DSB_DEBUG (so every elevator move prints "sb elev=N"), plus
# sb_probe which creates a console window and idles.  Injects a HELD down-arrow
# SELECT (down ... ~1.2 s ... up) into the WM's pointer sink via fake_terminal
# --inject-cpu, and asserts the elevator line-stepped MANY times — the initial
# click plus auto-repeats — proving the WaitAnyQueue-timeout repeat fires while
# an arrow is held.  Without auto-repeat there'd be only the press (+ the
# release repaint): two moves, not many.
#
# Geometry: the first window lands at (CELL_ORIGIN_X, CELL_ORIGIN_Y) = (8,16);
# the elevator starts at the top, so its down-arrow third is at window-local
# (~656, ~78) -> screen (664, 94).  The ▼ arrow (not ▲) is used because the
# elevator is pinned at the top, where ▲ is clamped.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
[ -f build/liborisc.ora ] || make -s lib >/dev/null
TMP=$(mktemp -d); trap "rm -rf $TMP" EXIT
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# --- build the WM (.orx) WITH -DSB_DEBUG ---------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" -DSB_DEBUG \
    -I tools/cc/arch/orisc -I tools/cc/lib ouroboros/oriscwm.c > "$TMP/wm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/wm.i" > "$TMP/wm.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/wm.s"                      -o "$TMP/wm.oro"
python3 tools/ld/orld -o "$TMP/oriscwm.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/wm.oro" build/liborisc.ora

# --- build sb_probe.orx ---------------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/sb_probe.c > "$TMP/sm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/sm.i" > "$TMP/sm.s"
python3 tools/asm/asmorisc -r "$TMP/sm.s"                      -o "$TMP/sm.oro"
python3 tools/ld/orld -o "$TMP/sb_probe.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sm.oro" build/liborisc.ora

# --- oriscbar + oriscdir --------------------------------------------------
SOCK="$TMP/bar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 & BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 -v > "$TMP/dir.out" 2>&1 & DIR=$!
for _ in $(seq 60); do grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break; sleep 0.05; done

# --- WM CPU0 (owns the pointer sink) -------------------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" --init-r4 1 \
    "$TMP/oriscwm.orx" > "$TMP/wm.out" 2>&1 & WM=$!
for _ in $(seq 200); do grep -q "oriscwm: ready" "$TMP/wm.out" 2>/dev/null && break; sleep 0.05; done

# --- sb_probe CPU1 (keeps a window alive) --------------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" --service "18=1@9" \
    "$TMP/sb_probe.orx" > "$TMP/cpu.out" 2>&1 & CPU=$!
for _ in $(seq 200); do grep -q "sb_probe: ready" "$TMP/cpu.out" 2>/dev/null && break; sleep 0.05; done

# --- inject a HELD down-arrow: down, ~1.2 s, up --------------------------
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 --inject-cpu 0 \
    --delay 1.2 --linger 4.0 \
    --event down:664,94,1 \
    --event up:664,94,1 \
    > "$TMP/term.out" 2>&1 & TERMP=$!

sleep 3.5    # the press + ~8 auto-repeats land between t=1.2 and t=2.4

kill -TERM $TERMP $CPU $WM $DIR $BAR 2>/dev/null || true
wait 2>/dev/null || true

# --- assert: many elevator moves (press + auto-repeats), trending down ---
N=$(grep -c "oriscwm: sb elev=" "$TMP/wm.out" || true)
FIRST=$(grep "oriscwm: sb elev=" "$TMP/wm.out" | head -1 | sed 's/.*elev=//' || true)
LAST=$(grep "oriscwm: sb elev=" "$TMP/wm.out"  | tail -1 | sed 's/.*elev=//' || true)
echo "elevator moves: $N   (first elev=$FIRST, last elev=$LAST)"

if [ "${N:-0}" -ge 4 ] && [ -n "${FIRST:-}" ] && [ -n "${LAST:-}" ] && [ "$LAST" -gt "$FIRST" ]; then
    echo "PASS: held ▼ arrow auto-repeated ($N line-steps, elevator $FIRST -> $LAST)"
    exit 0
fi
echo "FAIL: expected >=4 elevator moves trending down (press + auto-repeats)"
echo "--- WM out (tail) ---"; tail -20 "$TMP/wm.out" || true
exit 1

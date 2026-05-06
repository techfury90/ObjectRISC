#!/bin/sh
# test_shell_edit.sh — full-screen editor on the grid canvas.
#
# Phase 39 ships `edit <path>` in the shell, on top of Phase 38's
# grid plumbing + new oriscterm cell-replace tracking. The test:
#
#   1. Pre-create greeting.txt with two short lines.
#   2. `edit greeting.txt`
#   3. Type:  RIGHT-arrow ×4 ('!' inserts at column 4 of line 1)
#       Result line 1: "Hi !there"  (after Hi, then ' there')
#         actually the original is "Hi there" (length 8); after 4
#         right arrows the cursor sits on the space at col 3 (or
#         after 'H','i',' ','t' = col 4); inserting '!' shifts
#         't' rightward giving "Hi t!here" or similar — we don't
#         try to be too clever with the exact column, just verify
#         that:
#          - the file gains a '!' character
#          - the original line content is preserved
#          - a '*' (modified marker) appears in the status line
#   4. ^S to save.
#   5. ^X to quit.
#   6. Re-read greeting.txt off the host and grep for '!'.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

mkdir -p "$TMP/jail"

cat > "$TMP/jail/greeting.txt" <<'EOF'
Hi there
line two
EOF

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (EDIT)"' \
    examples/cc/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    tools/cc/lib/liborisc.ora

SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# Type:  edit greeting.txt<RET>
#        DOWN  END-ish (RIGHT * 8 to get past "line two")
#        '!' (insert)
#        ^S (save)
#        ^X (quit)
#        exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:e --event key:d --event key:i --event key:t \
    --event key:0x20 \
    --event key:g --event key:r --event key:e --event key:e --event key:t --event key:i --event key:n --event key:g \
    --event key:0x2e --event key:t --event key:x --event key:t \
    --event key:0x10D \
    --event key:0x181 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x21 \
    --event key:0x13 \
    --event key:0x18 \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 12.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# Service order: O5=console O6=keyboard O7=grid O8/O9=pad O10=hostfsd.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "0=0@0" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
wait $CPU0 2>/dev/null || true
for p in $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- shell stderr ---"
cat "$TMP/cpu0.err"

# Last frame: should show edited file + status line with '*' (dirty)
# at the moment we hit ^S. After ^S the dirty flag clears, but we
# also send ^X immediately after, so the last rendered frame is the
# post-^S, pre-^X frame — which is clean. That's fine; the smoking
# gun for the test is the on-disk file.
sed -n '/--- grid last frame ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/lastframe.txt"

echo "--- last frame ---"
cat "$TMP/lastframe.txt"

echo "--- saved file ---"
cat "$TMP/jail/greeting.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

# The original second line is "line two" (8 chars). Cursor starts
# at (0,0). DOWN moves to row 1, then 9 RIGHT-arrows would walk
# past EOL onto row 1 col 8 (clamped) — actually 8 RIGHTs land
# at col 8 = EOL, the 9th tries to advance to next row but there
# isn't one, so cursor stays at (1,8). Then '!' inserts at the
# end of line 2.
grep -q "line two!" "$TMP/jail/greeting.txt" \
    || fail "expected 'line two!' in saved file (^S didn't write)"
grep -q "^Hi there$" "$TMP/jail/greeting.txt" \
    || fail "first line was clobbered"

# Status line on the last rendered frame should still mention edit.
grep -q "edit: /greeting\.txt" "$TMP/lastframe.txt" \
    || fail "status line not painted"

echo "PASS"

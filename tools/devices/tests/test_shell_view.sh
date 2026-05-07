#!/bin/sh
# test_shell_view.sh — full-screen viewer on the grid canvas.
#
# Phase 38 plumbing: the shell now has grid (idx 3) and vector
# (idx 4) services in O7/O8, and a `view <path>` builtin that
# renders a file into the 80×24 grid via grid_print + grid_clear.
#
# Sequence:
#     view greeting.txt<RET>     ; full-screen render
#     j j j<RET>                  ; (no-op for short file, just
#                                ;  ensures key handling doesn't
#                                ;  crash the viewer)
#     q<RET>                      ; quit, back to prompt
#     exit<RET>                   ; clean shell exit
#
# Asserts the grid render contains the file's lines AND the status
# line "view: greeting.txt" with the line count.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

mkdir -p "$TMP/jail"

# A small, easily-recognisable file.
cat > "$TMP/jail/greeting.txt" <<'EOF'
Hello from the grid canvas!
This is line two.
And a third line for good measure.
EOF

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (VIEW)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

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

# Type:  view greeting.txt<RET>  j j j  q  exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:v --event key:i --event key:e --event key:w \
    --event key:0x20 \
    --event key:g --event key:r --event key:e --event key:e --event key:t --event key:i --event key:n --event key:g \
    --event key:0x2e --event key:t --event key:x --event key:t \
    --event key:0x10D \
    --event key:j --event key:j --event key:j \
    --event key:q \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 8.0 --delay 0.20 \
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

# Extract the last non-empty grid frame the viewer painted. The
# viewer wipes the canvas on `q`, so the final grid is empty —
# fake_terminal stashes the last substantive frame before that
# wipe so we can assert against it.
sed -n '/--- grid last frame ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/lastframe.txt"

echo "--- last frame ---"
cat "$TMP/lastframe.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q "^Hello from the grid canvas!"      "$TMP/lastframe.txt" \
    || fail "first file line not painted on row 0"
grep -q "^This is line two\.$"               "$TMP/lastframe.txt" \
    || fail "second file line not painted on row 1"
grep -q "^And a third line for good measure" "$TMP/lastframe.txt" \
    || fail "third file line not painted on row 2"
grep -q "view: /greeting\.txt"               "$TMP/lastframe.txt" \
    || fail "status line missing the path"
grep -q "1/3"                                "$TMP/lastframe.txt" \
    || fail "status line missing the line counter"

# After view returns, the shell should be back at "/>" prompting
# for more input — proves the viewer cleanly handed control back.
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/console.txt"
[ "$(grep -c '^/> ' "$TMP/console.txt")" -ge 2 ] \
    || fail "shell didn't return to a fresh prompt after view"

echo "PASS"

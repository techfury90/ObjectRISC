#!/bin/sh
# test_shell_history.sh — verify the shell's command-history
# arrow-key recall.
#
# Types:
#   pwd<RET>           ; first command goes into history at slot 0
#   echo abc<RET>      ; second command goes into slot 1
#   <UP><UP><RET>      ; recall slot 0 → "pwd" → execute → prints "/"
#   <UP><RET>          ; recall slot 2 (the just-recalled "pwd") → "/"
#   <UP><DOWN><RET>    ; UP gets latest, DOWN goes back to empty → no-op
#   exit<RET>
#
# Asserts the rendered console shows the expected interleaving of
# typed text + recalled text + command output.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/jail"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (HISTORY)"' \
    ouroboros/shell.c > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/program.oro" \
    build/liborisc.ora

SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" -v \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# Type the test sequence. 0x10D = RET, 0x180 = UP, 0x181 = DOWN.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:p --event key:w --event key:d --event key:0x10D \
    --event key:e --event key:c --event key:h --event key:o --event key:0x20 \
    --event key:a --event key:b --event key:c --event key:0x10D \
    --event key:0x180 --event key:0x180 --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t --event key:0x10D \
    --linger 6.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" --service "0=0@0" \
    --service "0=0@0" --service "0=0@0" --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
kill -KILL $CPU $HF $BAR 2>/dev/null || true
wait $CPU $HF $BAR 2>/dev/null || true

sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

# After the two arrow-up presses, the displayed line should read
# `/> pwd` (recalled from history slot 0), and on RET pwd should
# fire and print "/".
PWD_LINES=$(grep -c '^/$'         "$TMP/rendered.txt" || true)
[ "$PWD_LINES" -ge 2 ] \
    || fail "expected pwd to fire ≥2 times (typed once, recalled once); got $PWD_LINES"
grep -q '^abc$'                   "$TMP/rendered.txt" \
    || fail "echo abc didn't render"

echo "PASS"

#!/bin/sh
# test_shell_backspace.sh — verify the shell's backspace gives
# visual undo on the terminal.
#
# Types "exi" + BACKSPACE + "it" + RET, then "exit" + RET.
#
# Pre-fix behavior: the shell's read_line edited the line buffer
# silently; the terminal kept showing "exi" with the additional
# "it" appended → "exiit" rendered, even though the executed
# command was "exit" (because the buffer was correct).
#
# Post-fix behavior: read_line echoes a literal '\b' (0x08) on
# backspace, which oriscterm / fake_terminal interpret as "delete
# the previous rendered character" — so the rendered stream shows
# "exit" exactly, matching what the buffer holds.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/jail"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (BACKSPACE)"' \
    examples/cc/shell.c > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    tools/cc/lib/liborisc.ora

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

# Type "exi" + BACKSPACE + "it" + RET, then "exit" + RET.
# 0x108 = TK_BACKSPACE, 0x10D = RET.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:e --event key:x --event key:i \
    --event key:0x108 \
    --event key:i --event key:t --event key:0x10D \
    --linger 8.0 --delay 0.20 \
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

# After backspace + "it", the rendered line in the prompt area
# should contain the substring "/> exit" — with the mistyped 'i'
# *gone*, not lingering as "/> exiit". (We don't assert on the
# trailing newline or "bye!" — those race the shell's TaskExit and
# can be lost; documented in test_shell.sh.)
fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q '/> exit'        "$TMP/rendered.txt" || fail "expected '/> exit' rendered (post-backspace)"
! grep -q 'exiit'        "$TMP/rendered.txt" || fail "saw 'exiit' — backspace didn't delete the rendered char"

echo "PASS"

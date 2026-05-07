#!/bin/sh
# run_kbd_echo.sh — interactive demo of oriscterm's keyboard service.
#
# Spawns: oriscbar + oriscterm (Tk window) + a CPU running kbd_echo.
# The CPU subscribes to the terminal's keyboard service and prints
# every keystroke (codepoint + modifier names) to its host stdout —
# look for [cpu0] lines in this script's output. Press ESC in the
# terminal window to exit.
#
# The terminal's two service objects are at fixed (pid=16, gen=1):
#   idx 1 = console    — passed to the CPU as O5 via service=16=1@9
#   idx 2 = keyboard   — passed to the CPU as O6 via service=16=2@9
# The 0x09 caps are R|S — read + send-permitted, the minimum the
# terminal needs from the CPU's view.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Build the demo .orx through the now-standard pipeline.
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
    build/liborisc.ora

exec python3 tools/oriscrun \
    --terminal pid=16 \
    --cpu "pid=0:program=$TMP/kbd_echo.orx,service=16=1@9,service=16=2@9" \
    --leader 0 --leader-timeout 600

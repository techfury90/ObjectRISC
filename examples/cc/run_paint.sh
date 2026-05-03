#!/bin/sh
# run_paint.sh — interactive demo of oriscterm's keyboard + vector
# capabilities together. Pass keyboard at O6 and vector at O8 (the
# slot for the second through fourth --service flags lands at O5,
# O6, O7, O8 in order; service=16=1 → O5 console, =2 → O6 keyboard,
# =3 → O7 grid, =4 → O8 vector).
#
# Focus the Tk window and try the controls listed in paint.c.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/paint.c \
    > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/paint.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    tools/cc/lib/liborisc.ora

exec python3 tools/oriscrun \
    --terminal pid=16 \
    --cpu "pid=0:program=$TMP/paint.orx,service=16=1@9,service=16=2@9,service=16=3@9,service=16=4@9" \
    --leader 0 --leader-timeout 600

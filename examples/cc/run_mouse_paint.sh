#!/bin/sh
# run_mouse_paint.sh — interactive demo of oriscterm's pointer service.
#
# --service slot order: 16=4@9 → vector at O8, 16=6@9 → pointer at O9.
# (We skip console/keyboard/grid for this demo — the binary doesn't
# need them, and the boot ABI fills slots in --service order
# starting at O5.)
#
# So: O5=vector  O6=pointer.   No wait — five slots are filled in
# the order given: O5, O6, O7, O8, O9. That means we need to pad
# unused slots so the demo finds vector at O8 and pointer at O9
# (which is what mouse_paint.c expects). The pad is just any
# spare ref; we use the console at idx 1 three times.

set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
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

# Slot layout via --service (each spec lands at the next free O5..O15):
#   O5 = console   (16=1@9) — unused but reserves the slot
#   O6 = keyboard  (16=2@9) — unused but reserves the slot
#   O7 = grid      (16=3@9) — unused but reserves the slot
#   O8 = vector    (16=4@9)
#   O9 = pointer   (16=6@9)
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --cpu "pid=0:program=$TMP/mouse_paint.orx,service=16=1@9,service=16=2@9,service=16=3@9,service=16=4@9,service=16=6@9" \
    --leader 0 --leader-timeout 600

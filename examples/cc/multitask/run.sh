#!/bin/sh
# run.sh — build and run the multitask demo.
#
# Builds multitask.c through pcc → asmorisc → orld and runs the
# resulting .orx under simorisc. Output goes to host stdout via the
# legacy print_str / firmware ConsoleWrite path — no terminal or
# host-fs services needed for this demo.

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
    -I tools/cc/arch/orisc -I tools/cc/lib \
    examples/cc/multitask/multitask.c > "$TMP/multitask.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/multitask.i" > "$TMP/multitask.s"

python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/multitask.s"                -o "$TMP/multitask.oro"
python3 tools/ld/orld -o "$TMP/multitask.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/multitask.oro" \
    tools/cc/lib/liborisc.ora

exec python3 tools/sim/simorisc "$TMP/multitask.orx"

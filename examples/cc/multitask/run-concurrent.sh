#!/bin/sh
# run-concurrent.sh — build and run the concurrent-children demo.
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
    examples/cc/multitask/concurrent.c > "$TMP/concurrent.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/concurrent.i" > "$TMP/concurrent.s"

python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/concurrent.s"               -o "$TMP/concurrent.oro"
python3 tools/ld/orld -o "$TMP/concurrent.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/concurrent.oro" \
    tools/cc/lib/liborisc.ora

exec python3 tools/sim/simorisc "$TMP/concurrent.orx"

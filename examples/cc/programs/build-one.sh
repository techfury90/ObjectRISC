#!/bin/sh
# build-one.sh — compile and link one C source under examples/cc/programs/
# into a .orx that the shell can `run`. Used by run_shell.sh and by
# anyone adding a new demo program.
#
# Usage: examples/cc/programs/build-one.sh path/to/foo.c path/to/foo.orx

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 program.c output.orx" >&2
    exit 1
fi

src="$1"
out="$2"

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

"$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/p.i"
"$CCOM" < "$TMP/p.i" > "$TMP/p.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/p.s"                       -o "$TMP/p.oro"
python3 tools/ld/orld -o "$out" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/p.oro" \
    tools/cc/lib/liborisc.ora

echo "built $out"

#!/bin/sh
# test_manyargs.sh — regression for the orisc pcc backend's 5+-arg
# calling convention (toolchain Phase 1A).
#
# Compiles examples/cc/manyargs.c through the pcc pipeline and runs it
# on simorisc; main() returns 0 on success or a small code naming the
# first failed check. Guards against the FUNARG-spill / outgoing-arg-
# area-reservation bugs regressing.
#
# Requires the pcc binaries in $PCC_BUILD (build with tools/cc/build.sh).

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    echo "SKIP: pcc not built at $PCC_BUILD (run tools/cc/build.sh)" >&2
    exit 0
fi

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/manyargs.c \
    > "$TMP/p.i"
"$CCOM" < "$TMP/p.i" > "$TMP/p.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/p.s"                        -o "$TMP/p.oro"
python3 tools/ld/orld -o "$TMP/manyargs.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/p.oro" build/liborisc.ora

rc=0
python3 tools/sim/simorisc "$TMP/manyargs.orx" || rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PASS"
else
    echo "FAIL: manyargs returned $rc (first failing check; 0 = all pass)" >&2
    exit 1
fi

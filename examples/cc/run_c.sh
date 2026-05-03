#!/bin/sh
# run_c.sh — compile and run a C file through the orisc pcc port.
#
# Usage: run_c.sh path/to/program.c
#
# Pipeline: cpp -> ccom -> asmorisc -r (one .oro per .s) -> orld -> simorisc.
# Each translation unit is assembled separately and the linker (orld) merges
# them — this replaces the older "asmorisc concatenates everything" path,
# which couldn't distinguish per-unit local labels (L1, L2, …) and needed a
# sed-rename hack to disambiguate.

set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 program.c [simorisc args...]" >&2
    exit 1
fi

src="$1"
shift

cd "$(dirname "$0")/../.."

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    cat >&2 <<EOF
error: pcc binaries not found in $PCC_BUILD
       (looked for: $CPP and $CCOM)

Bootstrap the C compiler with:
    tools/cc/build.sh

Then re-run this script.
EOF
    exit 1
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

LIBORISC="tools/cc/lib/liborisc.ora"
if [ ! -f "$LIBORISC" ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

# 1) Compile the user's program (cpp + ccom). Both -I dirs:
#    arch/orisc for orisc.h (OR-file macros), lib for liborisc.h
#    (libc prototypes).
"$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/program.i"
"$CCOM"                                                < "$TMP/program.i" \
    > "$TMP/program.s"

# 2) Assemble each translation unit separately to a relocatable .oro.
#    asmorisc -r scopes pcc's L\d+ local labels per-file, so no name
#    collisions across units.
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                 -o "$TMP/program.oro"

# 3) Link with the libc archive. orld pulls in only the members
#    that satisfy unresolved externals — programs that don't call
#    print_int or strlen don't pay for them.
python3 tools/ld/orld -o "$TMP/program.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" "$LIBORISC"

# 4) Run.
exec python3 tools/sim/simorisc "$@" "$TMP/program.orx"

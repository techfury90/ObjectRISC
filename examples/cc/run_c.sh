#!/bin/sh
# run_c.sh — compile and run a C file through the orisc pcc port.
#
# Usage: run_c.sh path/to/program.c
#
# Pipeline: cpp -> ccom (orisc-targeted) -> asmorisc (linking
# crt0.s + console_io.s + program.s) -> simorisc.

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

Build pcc for orisc once:
    mkdir -p $PCC_BUILD && cd $PCC_BUILD
    $PWD/tools/cc/configure --target=orisc-unknown-none
    make
EOF
    exit 1
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Compile the user's program. Local-label numbering (L1, L2, ...) is
# per-compilation-unit but the assembler sees one big symbol table,
# so without a real linker we'd get duplicate-label errors when
# linking multiple .c files. sed-rename `L\d+` per-unit as a stand-in
# for proper local-symbol scoping until pcc grows a label-prefix
# flag or we ship a linker. (macOS sed doesn't support \b, so we
# anchor by ensuring nothing else in pcc's output starts with L
# followed by a digit — true for all our use cases.)
"$CPP"  "$src" > "$TMP/program.i"
"$CCOM" < "$TMP/program.i" | sed 's/L\([0-9][0-9]*\)/LP\1/g' > "$TMP/program.s"

# Compile the shared library helpers (print_str, print_int, ...) so
# every demo can use them without re-implementing.
"$CPP"  examples/cc/lib.c > "$TMP/lib.i"
"$CCOM" < "$TMP/lib.i" | sed 's/L\([0-9][0-9]*\)/LL\1/g' > "$TMP/lib.s"

python3 tools/asm/asmorisc \
    tools/cc/arch/orisc/crt0.s \
    tools/cc/arch/orisc/console_io.s \
    "$TMP/lib.s" \
    "$TMP/program.s" \
    -o "$TMP/program.orx"

exec python3 tools/sim/simorisc "$@" "$TMP/program.orx"

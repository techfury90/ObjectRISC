#!/bin/sh
# build.sh — compile every .c in this directory and bundle the .oro
# outputs into liborisc.ora.
#
# Output: tools/cc/lib/liborisc.ora (and one .oro per source file).
# Run from anywhere; the script self-locates.

set -eu

LIB_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$LIB_DIR/../../.." && pwd)"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    cat >&2 <<EOF
error: pcc binaries not found in $PCC_BUILD
       (looked for: $CPP and $CCOM)
EOF
    exit 1
fi

oros=""
for src in "$LIB_DIR"/*.c; do
    name=$(basename "$src" .c)
    obj="$LIB_DIR/$name.oro"
    "$CPP"  -I "$LIB_DIR" "$src" \
        | "$CCOM" \
        | python3 "$ROOT/tools/asm/asmorisc" -r /dev/stdin -o "$obj"
    oros="$oros $obj"
done

# Standalone .s files (handlers / glue) that can't easily be expressed
# as inline asm inside a C function — typically because pcc would emit
# a function prologue/epilogue around the label that interferes with
# the calling convention (e.g., a trap handler entered via a vector
# rather than a normal call).
for src in "$LIB_DIR"/*.s; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .s)
    obj="$LIB_DIR/$name.oro"
    python3 "$ROOT/tools/asm/asmorisc" -r "$src" -o "$obj"
    oros="$oros $obj"
done

# shellcheck disable=SC2086
python3 "$ROOT/tools/ld/oar" c "$LIB_DIR/liborisc.ora" $oros

echo "built $LIB_DIR/liborisc.ora ($(wc -c <"$LIB_DIR/liborisc.ora") bytes)"
python3 "$ROOT/tools/ld/oar" t "$LIB_DIR/liborisc.ora"
echo "symbols:"
python3 "$ROOT/tools/ld/oar" s "$LIB_DIR/liborisc.ora"

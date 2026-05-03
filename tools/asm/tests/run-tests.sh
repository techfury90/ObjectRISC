#!/bin/sh
# tests/run-tests.sh — assemble each test .s and verify both:
#   * the raw .orx round-trips through --disasm and reassembles bit-identically
#   * the --hex output matches the expected text file
#
# Run from anywhere; the script self-locates.

set -u

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
ASM="$TESTS_DIR/../asmorisc"
TMP="${TMPDIR:-/tmp}"

pass=0
fail=0
failed_names=""

for src in "$TESTS_DIR"/*.s; do
    name="$(basename "$src" .s)"
    expected="$TESTS_DIR/$name.expected"
    if [ ! -f "$expected" ]; then
        printf 'SKIP %-32s (no .expected file)\n' "$name"
        continue
    fi
    out_orx="$TMP/asmorisc-test-$name.orx"
    actual_hex="$TMP/asmorisc-test-$name.hex"

    # Assemble
    if ! python3 "$ASM" "$src" -o "$out_orx" 2>"$TMP/asmorisc-err.$$"; then
        printf 'FAIL %-32s (assemble error)\n' "$name"
        cat "$TMP/asmorisc-err.$$"
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        continue
    fi

    # Hex dump
    python3 "$ASM" --hex "$out_orx" > "$actual_hex" 2>>"$TMP/asmorisc-err.$$"

    if ! diff -u "$expected" "$actual_hex" > "$TMP/asmorisc-diff.$$" 2>&1; then
        printf 'FAIL %-32s\n' "$name"
        cat "$TMP/asmorisc-diff.$$"
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        continue
    fi

    # Round-trip: --disasm output should reassemble to byte-identical .orx
    rt_asm="$TMP/asmorisc-test-$name.rt.s"
    rt_orx="$TMP/asmorisc-test-$name.rt.orx"
    python3 "$ASM" --disasm "$out_orx" > "$rt_asm"
    if ! python3 "$ASM" "$rt_asm" -o "$rt_orx" 2>"$TMP/asmorisc-err.$$"; then
        printf 'FAIL %-32s (disasm reassembly error)\n' "$name"
        cat "$TMP/asmorisc-err.$$"
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        continue
    fi
    if ! cmp -s "$out_orx" "$rt_orx"; then
        printf 'FAIL %-32s (round-trip mismatch)\n' "$name"
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        continue
    fi

    printf 'PASS %-32s\n' "$name"
    pass=$((pass + 1))
done

rm -f "$TMP/asmorisc-err.$$" "$TMP/asmorisc-diff.$$"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
if [ "$fail" -gt 0 ]; then
    printf 'failed:%s\n' "$failed_names"
    exit 1
fi
exit 0

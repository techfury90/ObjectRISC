#!/bin/sh
# tools/ld/tests/run-tests.sh — run each *.sh in this directory and
# count pass/fail. Each test script is self-contained: it builds its
# own .oro files in a tempdir and asserts on simorisc exit codes or
# orld error messages.

set -u

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0
failed=""

for t in "$TESTS_DIR"/*.sh; do
    name=$(basename "$t" .sh)
    [ "$name" = "run-tests" ] && continue
    if sh "$t" >"$TESTS_DIR/.last.out" 2>"$TESTS_DIR/.last.err"; then
        printf 'PASS %-32s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL %-32s\n' "$name"
        echo "--- stdout ---"
        cat "$TESTS_DIR/.last.out"
        echo "--- stderr ---"
        cat "$TESTS_DIR/.last.err"
        fail=$((fail + 1))
        failed="$failed $name"
    fi
done

rm -f "$TESTS_DIR/.last.out" "$TESTS_DIR/.last.err"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || { printf 'failed:%s\n' "$failed"; exit 1; }
exit 0

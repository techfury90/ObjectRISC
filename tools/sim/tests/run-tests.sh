#!/bin/sh
# run-tests.sh — exercise simorisc against the hand-encoded hello world.
#
# 1. Regenerate tests/hello-test.orx from build-hello-test.py (this proves
#    our encoding still agrees with CONTRACT.md).
# 2. Run simorisc on it.
# 3. Verify stdout is exactly "Hello, world!\n" and exit code is 0.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
SIM="$(cd "$DIR/.." && pwd)/simorisc"

# Regenerate the binary.
python3 "$DIR/build-hello-test.py" > /dev/null

# Run the simulator, capturing stdout to a file (so we preserve the exact
# bytes — including any trailing newline — without going through shell
# command substitution, which would strip them). We invoke via python3 so
# the test works whether or not `simorisc` has been made executable.
ACTUAL_FILE="$(mktemp)"
trap 'rm -f "$ACTUAL_FILE"' EXIT
set +e
python3 "$SIM" "$DIR/hello-test.orx" > "$ACTUAL_FILE"
RC=$?
set -e

# Compare by hex so trailing-newline semantics are unambiguous.
ACTUAL_HEX=$(od -An -tx1 < "$ACTUAL_FILE" | tr -d ' \n')
EXPECTED_HEX=$(printf 'Hello, world!\n' | od -An -tx1 | tr -d ' \n')

if [ "$RC" -ne 0 ]; then
    echo "FAIL: simorisc exited with status $RC" >&2
    exit 1
fi

if [ "$ACTUAL_HEX" != "$EXPECTED_HEX" ]; then
    echo "FAIL: stdout mismatch" >&2
    echo "  expected (hex): $EXPECTED_HEX" >&2
    echo "  got      (hex): $ACTUAL_HEX" >&2
    exit 1
fi

echo "PASS: hello-test"

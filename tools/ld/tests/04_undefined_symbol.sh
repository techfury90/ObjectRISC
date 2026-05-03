#!/bin/sh
# Linker should error out on an unresolved external reference.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.text
_start:
    jal   does_not_exist      ; reference to a symbol nobody defines
    nop
    call  #0x001
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/a.s" -o "$TMP/a.oro"
set +e
err=$(python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/a.oro" 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "expected non-zero exit from orld, got 0" >&2; exit 1; }
echo "$err" | grep -q "undefined reference to 'does_not_exist'" \
    || { echo "expected 'undefined reference to does_not_exist', got: $err" >&2; exit 1; }

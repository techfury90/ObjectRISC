#!/bin/sh
# Linker should error out on a global symbol defined in two .oro files.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.text
_start:
    jal   helper
    nop
    addiu r4, r2, 0
    call  #0x001
    nop

helper:
    addiu r2, r0, 1
    jr    r31
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.text
helper:
    addiu r2, r0, 2
    jr    r31
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/a.s" -o "$TMP/a.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/b.s" -o "$TMP/b.oro"
set +e
err=$(python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/a.oro" "$TMP/b.oro" 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "expected non-zero exit from orld, got 0" >&2; exit 1; }
echo "$err" | grep -q "duplicate definition of global symbol 'helper'" \
    || { echo "expected duplicate-global error, got: $err" >&2; exit 1; }

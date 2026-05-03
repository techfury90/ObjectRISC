#!/bin/sh
# Linking against an archive: pull in a member that defines `helper`.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/main.s" <<'EOF'
.entry _start
.text
_start:
    jal   helper
    nop
    addiu r4, r2, 0
    call  #0x001
    nop
EOF

cat >"$TMP/lib_a.s" <<'EOF'
.text
helper:
    addiu r2, r0, 77
    jr    r31
    nop
EOF

cat >"$TMP/lib_b.s" <<'EOF'
.text
unused:
    addiu r2, r0, 99
    jr    r31
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/main.s"  -o "$TMP/main.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/lib_a.s" -o "$TMP/lib_a.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/lib_b.s" -o "$TMP/lib_b.oro"
python3 "$ROOT/tools/ld/oar" c "$TMP/lib.ora" "$TMP/lib_a.oro" "$TMP/lib_b.oro"
python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/main.oro" "$TMP/lib.ora"
set +e
python3 "$ROOT/tools/sim/simorisc" "$TMP/out.orx"
rc=$?
set -e
[ "$rc" -eq 77 ] || { echo "expected exit 77, got $rc" >&2; exit 1; }

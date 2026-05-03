#!/bin/sh
# Transitive pull-in: main needs A, A needs B. The linker should pull
# in B from the archive after pulling in A, on a second iteration.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/main.s" <<'EOF'
.entry _start
.text
_start:
    jal   a_func              ; needs A
    nop
    addiu r4, r2, 0
    call  #0x001
    nop
EOF

cat >"$TMP/a.s" <<'EOF'
.text
a_func:
    addu  r17, r31, r0
    jal   b_func              ; A needs B
    nop
    addu  r31, r17, r0
    addiu r2, r2, 1           ; b returns 12, we return 13
    jr    r31
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.text
b_func:
    addiu r2, r0, 12
    jr    r31
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/main.s" -o "$TMP/main.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/a.s"    -o "$TMP/a.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/b.s"    -o "$TMP/b.oro"
python3 "$ROOT/tools/ld/oar" c "$TMP/lib.ora" "$TMP/a.oro" "$TMP/b.oro"
python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/main.oro" "$TMP/lib.ora"
set +e
python3 "$ROOT/tools/sim/simorisc" "$TMP/out.orx"
rc=$?
set -e
[ "$rc" -eq 13 ] || { echo "expected exit 13, got $rc" >&2; exit 1; }

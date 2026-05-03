#!/bin/sh
# `.local NAME` makes a non-L\d+ name file-private, so two .oro files can
# both define `helper` without collision — provided each is .local.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.local helper
.text
_start:
    jal   helper              ; resolves to a.s's helper
    nop
    addiu r4, r2, 0
    jal   call_b              ; calls into b.s
    nop
    addiu r4, r2, 0
    call  #0x001
    nop

helper:
    addiu r2, r0, 7
    jr    r31
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.local helper
.text
call_b:
    addu  r17, r31, r0        ; preserve return address
    jal   helper              ; resolves to b.s's helper
    nop
    addu  r31, r17, r0
    jr    r31
    nop

helper:
    addiu r2, r0, 99
    jr    r31
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/a.s" -o "$TMP/a.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/b.s" -o "$TMP/b.oro"
python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/a.oro" "$TMP/b.oro"
set +e
python3 "$ROOT/tools/sim/simorisc" "$TMP/out.orx"
rc=$?
set -e
# main path: a.helper returns 7 -> r4 = 7 -> jal call_b -> b.helper returns 99
# -> r2 = 99 -> r4 = 99 -> TaskExit 99.
[ "$rc" -eq 99 ] || { echo "expected exit 99, got $rc" >&2; exit 1; }

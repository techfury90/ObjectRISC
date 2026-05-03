#!/bin/sh
# An unused archive member must NOT be pulled into the link. We verify
# this by giving the unused member a deliberately-broken symbol that
# would conflict with main's `_start` if it ever got linked in.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/main.s" <<'EOF'
.entry _start
.text
_start:
    jal   used_helper
    nop
    addiu r4, r2, 0
    call  #0x001
    nop
EOF

cat >"$TMP/lib_used.s" <<'EOF'
.text
used_helper:
    addiu r2, r0, 5
    jr    r31
    nop
EOF

cat >"$TMP/lib_poison.s" <<'EOF'
; If this member is pulled in (it shouldn't be), the duplicate `_start`
; will trip orld's duplicate-global check and fail the link.
.text
_start:
    addiu r2, r0, 99
    jr    r31
    nop
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/main.s"        -o "$TMP/main.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/lib_used.s"    -o "$TMP/lib_used.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/lib_poison.s"  -o "$TMP/lib_poison.oro"
python3 "$ROOT/tools/ld/oar" c "$TMP/lib.ora" \
    "$TMP/lib_used.oro" "$TMP/lib_poison.oro"
python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/main.oro" "$TMP/lib.ora"
set +e
python3 "$ROOT/tools/sim/simorisc" "$TMP/out.orx"
rc=$?
set -e
[ "$rc" -eq 5 ] || { echo "expected exit 5, got $rc" >&2; exit 1; }

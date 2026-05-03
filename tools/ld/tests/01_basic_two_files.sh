#!/bin/sh
# Two .s files, cross-file `jal`. The first defines `_start` and calls
# `compute`; the second defines `compute` which returns 42 in r2.
# Expect exit 42.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.text
_start:
    jal   compute
    nop
    addiu r4, r2, 0           ; TaskExit takes exit code in r4
    call  #0x001
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.text
compute:
    addiu r2, r0, 42          ; return 42 in r2
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
[ "$rc" -eq 42 ] || { echo "expected exit 42, got $rc" >&2; exit 1; }

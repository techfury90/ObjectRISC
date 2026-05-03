#!/bin/sh
# Two .s files BOTH define a label `L1`. With the linker's per-.oro
# scoping (L\d+ defaults to local), this should link cleanly. Without
# scoping it would error on duplicate symbol.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.text
_start:
    addiu r4, r0, 0           ; r4 starts at 0
    j     L1                  ; jump to a.s's L1 (local)
    nop
L1:
    addiu r4, r4, 11          ; r4 = 11
    jal   helper              ; cross-file (helper is global)
    nop
    call  #0x001
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.text
helper:
    j     L1                  ; jump to b.s's L1 (local — different from a's!)
    nop
L1:
    addiu r4, r4, 31          ; r4 += 31  → r4 = 42
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

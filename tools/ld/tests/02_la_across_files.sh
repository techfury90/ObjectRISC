#!/bin/sh
# `la rd, label` across files. The data lives in b.s; a.s loads its
# address via `la` and reads the first byte. The byte is 0x37 (= 55);
# verify the loader-resolved HI16/LO16 patches reconstruct the right
# absolute VA.

set -eu
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)

cat >"$TMP/a.s" <<'EOF'
.entry _start
.text
_start:
    la    r1, magic_byte      ; address of byte 0x37 in b.s's .data
    lb    r4, 0(r1)           ; r4 = 0x37 = 55
    nop
    call  #0x001              ; TaskExit r4
    nop
EOF

cat >"$TMP/b.s" <<'EOF'
.data
magic_byte:
    .byte 0x37
EOF

python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/a.s" -o "$TMP/a.oro"
python3 "$ROOT/tools/asm/asmorisc" -r "$TMP/b.s" -o "$TMP/b.oro"
python3 "$ROOT/tools/ld/orld" -o "$TMP/out.orx" "$TMP/a.oro" "$TMP/b.oro"
set +e
python3 "$ROOT/tools/sim/simorisc" "$TMP/out.orx"
rc=$?
set -e
[ "$rc" -eq 55 ] || { echo "expected exit 55, got $rc" >&2; exit 1; }

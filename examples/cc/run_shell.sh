#!/bin/sh
# run_shell.sh — interactive launcher for the Object RISC shell.
#
# Spawns:
#   - oriscbar (crossbar)
#   - oriscterm (Tk terminal, pid 16)
#   - hostfsd (host FS access, pid 17, jailed to repo root)
#   - linkbootd (program-spawn server, pid 18)
#   - shell CPU (pid 0; the leader — when it exits, everything else
#                tears down)
#   - 4 spare CPUs (pids 32..35) running the chunkboot loader; these
#                  are the program slots the shell's `run` command
#                  fills in by asking linkbootd to load a .orx
#
# The shell announces itself with the current real-world date minus
# 40 years (alternate-history conceit) — computed at build time and
# passed through cpp via -DBUILD_BANNER.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Compute the build banner. macOS's `date -v -40y` does the year
# arithmetic; on Linux use `date -d "40 years ago"`. `%-l` strips the
# leading-space-padded hour; `tr -s ' '` collapses the day-of-month
# space-padding from `%e`.
if date -v -40y +"%Y" >/dev/null 2>&1; then
    PAST=$(date -v -40y +"%b %e %Y %-l:%M %p" | tr -s ' ')
else
    PAST=$(date -d "40 years ago" +"%b %-d %Y %-l:%M %p")
fi
BANNER="Object RISC Shell ($PAST)"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# ---- compile the shell ----------------------------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER="\"$BANNER\"" \
    examples/cc/shell.c > "$TMP/shell.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                    -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/shell.oro" \
    tools/cc/lib/liborisc.ora

# ---- generate + assemble the chunkboot loader (one .orx, baked
#      into all 4 pool CPUs as their boot image) -----------------------
python3 examples/linkboot/gen_chunkboot.py >/dev/null
python3 tools/asm/asmorisc examples/linkboot/chunkboot.s -o "$TMP/chunkboot.orx"

# --service slot order (each spec lands at the next free O5..O15):
#   shell:
#     O5 = oriscterm console  (16=1@9)
#     O6 = oriscterm keyboard (16=2@9)
#     O7 = linkbootd          (18=1@9) — replaces a former pad slot
#     O8..O9 = unused (pad)
#     O10 = hostfsd           (17=1@9)
#   pool CPU (chunkboot loader; loader hijacks O6 as scratch then
#   shifts the rest down so the guest sees the standard ABI):
#     O5  = linkbootd  (18=1@9)  — master for chunked-boot protocol
#     O6  = pad        (0=0@0)   — loader uses for R+S self-ref
#     O7  = console    (16=1@9)  — guest's O5 after shift
#     O8  = keyboard   (16=2@9)  — guest's O6 after shift
#     O9  = pad        (0=0@0)   — guest's O7
#     O10 = pad        (0=0@0)   — guest's O8
#     O11 = hostfsd    (17=1@9)  — guest's O10 after shift (O9 ends up null)
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --hostfsd "pid=17,root=$ROOT" \
    --linkbootd "pid=18,shells=0,loaders=32;33;34;35,root=$ROOT" \
    --cpu "pid=0:program=$TMP/shell.orx,service=16=1@9,service=16=2@9,service=18=1@9,service=0=0@0,service=0=0@0,service=17=1@9" \
    --cpu "pid=32:program=$TMP/chunkboot.orx,service=18=1@9,service=0=0@0,service=16=1@9,service=16=2@9,service=0=0@0,service=0=0@0,service=17=1@9,reset" \
    --cpu "pid=33:program=$TMP/chunkboot.orx,service=18=1@9,service=0=0@0,service=16=1@9,service=16=2@9,service=0=0@0,service=0=0@0,service=17=1@9,reset" \
    --cpu "pid=34:program=$TMP/chunkboot.orx,service=18=1@9,service=0=0@0,service=16=1@9,service=16=2@9,service=0=0@0,service=0=0@0,service=17=1@9,reset" \
    --cpu "pid=35:program=$TMP/chunkboot.orx,service=18=1@9,service=0=0@0,service=16=1@9,service=16=2@9,service=0=0@0,service=0=0@0,service=17=1@9,reset" \
    --leader 0 --leader-timeout 600

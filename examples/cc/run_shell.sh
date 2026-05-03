#!/bin/sh
# run_shell.sh — interactive launcher for the Object RISC shell.
#
# Spawns oriscbar + oriscterm + hostfsd (jailed to the repo root)
# + a CPU running shell.c. The Tk window is the shell's stdin/stdout.
#
# The shell announces itself with the current real-world date
# minus 40 years (alternate-history conceit) — computed at build
# time and passed through cpp via -DBUILD_BANNER.

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
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER="\"$BANNER\"" \
    examples/cc/shell.c > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    tools/cc/lib/liborisc.ora

# --service slot order (each spec lands at the next free O5..O15):
#   O5 = oriscterm console  (16=1@9)
#   O6 = oriscterm keyboard (16=2@9)
#   O7..O9 = unused but reserved (we pad with throwaway refs)
#   O10 = hostfsd            (17=1@9)
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --hostfsd "pid=17,root=$ROOT" \
    --cpu "pid=0:program=$TMP/shell.orx,service=16=1@9,service=16=2@9,service=0=0@0,service=0=0@0,service=0=0@0,service=17=1@9" \
    --leader 0 --leader-timeout 600

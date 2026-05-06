#!/bin/sh
# run_shell.sh — interactive launcher for the Object RISC shell.
#
# Phase 30 architecture: the shell IS the supervisor. cmd_run loads
# a .orx via hostfsd and TaskCreates it as a child task on the same
# CPU. No spare-CPU pool, no linkbootd.
#
# Spawns:
#   - oriscbar (crossbar)
#   - oriscterm (Tk terminal, pid 16)
#   - hostfsd (host FS access, pid 17, jailed to repo root)
#   - shell CPU (pid 0; the leader — when it exits, everything else
#                tears down)
#
# The shell announces itself with the current real-world date minus
# 40 years (alternate-history conceit) — computed at build time and
# passed through cpp via -DBUILD_BANNER.
#
# Programs you can `run` from inside the shell live under
# examples/cc/programs/ in the repo (and so are visible inside the
# hostfsd jail at "/programs/..."). examples/cc/programs/README.md
# lists what's there and how to add more.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Build any .orx programs under examples/cc/programs/ so the shell
# can `run /programs/foo.orx` immediately after launch. Idempotent;
# rebuilds only what's missing.
PROGRAMS_DIR="$ROOT/examples/cc/programs"
if [ -d "$PROGRAMS_DIR" ]; then
    for src in "$PROGRAMS_DIR"/*.c; do
        [ -f "$src" ] || continue
        out="${src%.c}.orx"
        if [ ! -f "$out" ] || [ "$src" -nt "$out" ]; then
            bash examples/cc/programs/build-one.sh "$src" "$out" >/dev/null
        fi
    done
fi

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

# --service slot order (each spec lands at the next free O5..O15):
#   O5  = oriscterm console  (16=1@9)
#   O6  = oriscterm keyboard (16=2@9)
#   O7  = oriscterm grid     (16=3@9)  — Phase 38, used by cmd_view +
#                                       grid_clear (the grid service
#                                       handles both paint and clear)
#   O8  = pad — claimed at runtime by hf_init for its private mailbox
#   O9  = pad
#   O10 = hostfsd            (17=1@9)
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --hostfsd "pid=17,root=$ROOT" \
    --cpu "pid=0:program=$TMP/shell.orx,service=16=1@9,service=16=2@9,service=16=3@9,service=0=0@0,service=0=0@0,service=17=1@9" \
    --leader 0 --leader-timeout 600

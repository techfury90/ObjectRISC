#!/bin/sh
# boot.sh — start Ouroboros: the OS layer atop Object RISC.
#
# Phase 45a (this version): the shell is still CPU 0's direct
# leader. The supervisor program exists in the tree
# (ouroboros/supervisor.c, builds to build/supervisor.orx) and the
# libc has the spawn-RPC client (sup.c::sup_spawn) but the boot
# leader is still shell.orx so the existing test harnesses keep
# passing on the fallback path. Phase 45b will flip the leader
# once the wire-protocol edge cases are nailed down.
#
# Spawned processes by `make boot`:
#   - oriscbar  (crossbar)
#   - oriscterm (Tk terminal, pid 16)
#   - hostfsd   (host FS access, pid 17, jailed to repo root)
#   - the shell CPU (pid 0; the leader — when it exits, everything
#                    else tears down)
#
# The shell announces itself with the current real-world date minus
# 40 years (alternate-history conceit) — computed here, passed via
# SHELL_BUILD_BANNER to make.
#
# Programs you can `run` from inside the shell live under
# ouroboros/programs/ — built by `make programs` into build/programs/
# and visible inside the hostfsd jail at "/programs/..." because
# build/programs is symlinked into the jail at the start of this
# script. The shell itself is built to build/programs/shell.orx;
# boot.sh resolves directly to that file rather than going through
# the symlinked /programs path.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Compute today-minus-40 and rebuild the shell with that banner.
if date -v -40y +"%Y" >/dev/null 2>&1; then
    PAST=$(date -v -40y +"%b %e %Y %-l:%M %p" | tr -s ' ')
else
    PAST=$(date -d "40 years ago" +"%b %-d %Y %-l:%M %p")
fi
BANNER="Object RISC Shell ($PAST)"

# Force a shell rebuild with the computed banner. `make` would
# normally cache shell.orx with whatever SHELL_BUILD_BANNER it was
# last built with; touch the source to defeat that and re-derive
# the banner each boot.
touch ouroboros/shell.c
make -s shell SHELL_BUILD_BANNER="\"$BANNER\""
make -s programs

# The hostfsd jail expects programs at /programs/. We keep build
# artefacts under build/, so symlink the built programs in.
if [ ! -L "$ROOT/programs" ]; then
    rm -rf "$ROOT/programs"
    ln -s build/programs "$ROOT/programs"
fi

# --service slot order (each spec lands at the next free O5..O15):
#   O5  = oriscterm console  (16=1@9)
#   O6  = oriscterm keyboard (16=2@9)
#   O7  = oriscterm grid     (16=3@9)
#   O8  = pad (supervisor sub-cap when 45b flips the leader; null today)
#   O9  = pad
#   O10 = hostfsd            (17=1@9)
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --hostfsd "pid=17,root=$ROOT" \
    --cpu "pid=0:program=$ROOT/build/programs/shell.orx,service=16=1@9,service=16=2@9,service=16=3@9,service=0=0@0,service=0=0@0,service=17=1@9" \
    --leader 0 --leader-timeout 600

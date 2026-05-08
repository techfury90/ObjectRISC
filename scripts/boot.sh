#!/bin/sh
# boot.sh — start Ouroboros: the OS layer atop Object RISC.
#
# Phase 45f (this version): peer supervisors discover each other
# through the directory daemon (oriscdir) rather than via static
# --service slots. Each supervisor's boot O8 carries the directory
# mailbox sub-cap; supervisor.c copies it into DIR_SLOT and registers
# itself at /sys/cpu/<PROCID>/supervisor. Cross-CPU `run @N` walks
# /sys/cpu/N/supervisor to find the relay target. Removing the static
# wiring unblocks future hot-attached CPUs and self-registering
# devices (no boot script edit required to add another node).
#
# Phase 45c lineage: every CPU boots the same supervisor.orx, but
# only PROCID 0 (the leader) spawns a shell as its first user task —
# workers sit in their dispatch loop ready to service spawn requests.
# The leader's shell drives all visible activity; workers handle
# relayed spawn requests from peers. When the leader exits,
# oriscrun's `--leader 0` tears down the workers via SIGTERM.
#
# Phase 45b lineage: supervisor.orx allocates its own spawn-service
# mailbox at boot, derives a sub-cap into ORX_SLOT_CHILD_O8 so every
# TaskCreate it does injects the cap into the child's O8 (the libc
# task_init then harvests it into BOOT_PARENT_SLOT). All `run`/`edit`
# from a shell go through sup_spawn → SEND-RPC → its local supervisor's
# orx_spawn. The shell's `exit` SENDs op=2 (shutdown) so the
# leader supervisor can wind down without polling. Existing
# single-CPU test harnesses still launch shell.orx directly (no
# supervisor in O8 → sup_spawn falls back to orx_spawn).
#
# Spawned processes by `make boot`:
#   - oriscbar  (crossbar)
#   - oriscterm (Tk terminal, pid 16)
#   - hostfsd   (host FS access, pid 17, jailed to repo root)
#   - oriscdir  (directory daemon, pid 18)
#   - 2 supervisor CPUs (pid 0 = leader with shell; pid 1 = worker)
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
make -s all

# The hostfsd jail expects programs at /programs/. We keep build
# artefacts under build/, so symlink the built programs in.
if [ ! -L "$ROOT/programs" ]; then
    rm -rf "$ROOT/programs"
    ln -s build/programs "$ROOT/programs"
fi

# --service slot order (each spec lands at the next free O5..O15):
#   O5  = oriscterm console  (16=1@9 on CPU 0; 19=1@9 on CPU 1 — Phase 46)
#   O6  = oriscterm keyboard (16=2@9 / 19=2@9)
#   O7  = oriscterm grid     (16=3@9 / 19=3@9)
#   O8  = directory mailbox sub-cap (Phase 45f). oriscdir's primary
#        mailbox lives at descriptor idx 1, generation 1 (its first
#        ObjAlloc) — synthesize the cap as `18=1@9`. supervisor.c
#        harvests this into BOOT_PARENT_SLOT (via task_init's O8
#        snapshot) and copies it into DIR_SLOT directly at boot,
#        then dir_register's itself at /sys/cpu/<PROCID>/supervisor
#        so peer supervisors can dir_walk to it. Replaces 45e's
#        static peer-supervisor wiring; the directory now mediates
#        cross-CPU relay target discovery.
#   O9  = pad (null at boot; the supervisor's own freshly-allocated
#        spawn mailbox replaces this slot via `omov o9, o1`)
#   O10 = hostfsd            (17=1@9)
#
# Phase 46 — multi-terminal: each CPU is wired to its OWN oriscterm
# instance. CPU 0 → terminal pid 16, CPU 1 → terminal pid 19. Each
# CPU's supervisor checks O5 at boot (has_terminal probe), spawns a
# shell if non-null, and registers /sys/term/<procid>/{console,
# keyboard,grid} as service-discovery LEAFs. Result: two Tk windows,
# two independent shells, both sharing /programs and the same
# directory tree.
#
# hostfsd (17) and oriscdir (18) stay shared singletons — both CPUs
# see the same ones. /sys/hostfsd/0 is registered by CPU 0 only
# (procid==0 owns singletons today).
exec python3 tools/oriscrun \
    --terminal pid=16 \
    --terminal pid=19 \
    --hostfsd "pid=17,root=$ROOT" \
    --directory pid=18 \
    --cpu "pid=0:program=$ROOT/build/supervisor.orx,service=16=1@9,service=16=2@9,service=16=3@9,service=18=1@9,service=0=0@0,service=17=1@9" \
    --cpu "pid=1:program=$ROOT/build/supervisor.orx,service=19=1@9,service=19=2@9,service=19=3@9,service=18=1@9,service=0=0@0,service=17=1@9" \
    --leader 0 --leader-timeout 600

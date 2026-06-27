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
# Spawned processes by `make boot` (co-resident terminal):
#   - oriscbar  (crossbar)
#   - oriscdir  (directory daemon, pid 18)
#   - hostfsd   (host FS access, pid 17, jailed to repo root; serves
#               /programs + /fonts)
#   - 1 CPU (pid 0) running termfw.orx -> supervisor -> sysinit -> window
#     manager, all co-resident.  Apps launch from the WM's right-click menu.
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

# The co-resident chain spawns its system images from /programs (firmware ->
# /programs/supervisor.orx; sysinit -> /programs/oriscwm.orx).  They build into
# build/ (where the device tests consume them); mirror them into the /programs
# view the WM-menu apps already use.
cp -f build/supervisor.orx build/oriscwm.orx build/programs/

# The hostfsd jail expects programs at /programs/. We keep build
# artefacts under build/, so symlink the built programs in.
if [ ! -L "$ROOT/programs" ]; then
    rm -rf "$ROOT/programs"
    ln -s build/programs "$ROOT/programs"
fi

# Phase 47 — directory-driven boot. The only OR ref each CPU's
# firmware needs to wire is O8 = oriscdir's primary mailbox sub-cap.
# Everything else (terminals, hostfsd) is discovered at runtime via
# directory walks:
#
#     /sys/cpu/<procid>/supervisor — registered by each supervisor
#                                    at startup (Phase 45f)
#     /sys/term/<instance>/console
#                          /keyboard
#                          /grid    — registered by oriscterm itself
#                                    via the inline-register wire
#                                    op (Phase 47)
#     /sys/hostfsd/<instance>      — registered by hostfsd
#     /programs                    — mounted by the leader supervisor
#                                    (procid==0) onto hostfsd
#
# --service slot semantics with the new boot ABI:
#   O5..O7 = null pads. The supervisor's directory-walk-init populates
#            them from /sys/term/<procid>/{console,keyboard,grid} once
#            oriscterm has self-registered. has_terminal=true from the
#            walk gates shell-spawn and op=2 acceptance.
#   O8     = directory mailbox sub-cap (18=1@9). The single irreducible
#            wire — without this the supervisor has nothing to walk.
#            (A real implementation's firmware would inject this at
#            CPU bring-up; in the simulator we synthesize it via
#            simorisc's --service flag.)
#   O9     = pad (supervisor allocates its own spawn mailbox here).
#   O10    = null pad. Walked from /sys/hostfsd/0 by the supervisor.
#
# Co-resident terminal: instead of separate WM CPUs that supervisors
# discover over the wire, ONE CPU (pid 0) runs termfw.orx, which boots a
# framebuffer + splash and orx_spawns the supervisor as a same-CPU task;
# the supervisor spawns sysinit, which launches oriscwm -- all co-resident.
# The WM ADOPTS the firmware's framebuffer (signalled by a non-null O5 at
# the supervisor's boot, vs the null pad above) instead of allocating its
# own, and owns local keyboard/pointer sinks fed by simorisc's Tk worker --
# so the splash dissolves straight into the desktop in ONE window.  The WM
# registers at /sys/wm/0/0; apps launch from its right-click menu.  The
# legacy separate-CPU topology still works (guarded by the same null/non-
# null O5 split) and lives in the device tests (test_wm_boot.sh).
# Display: a Tk window by default; set OROS_NODISPLAY=1 for a headless boot.
DISPLAY_OPT=",display=tk"
[ -n "${OROS_NODISPLAY:-}" ] && DISPLAY_OPT=""
exec python3 tools/oriscrun \
    --directory pid=18 \
    --hostfsd "pid=17,instance=0,root=$ROOT" \
    --cpu "pid=0:program=$ROOT/build/termfw.orx,service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,service=0=0@0,service=0=0@0$DISPLAY_OPT" \
    --leader 0 --leader-timeout 600

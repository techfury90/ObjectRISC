#!/bin/sh
# boot-base.sh — bring up the base system with NO terminals: the crossbar
# (oriscbar), the directory (oriscdir, pid 18), hostfsd (pid 17), and two
# headless compute supervisors (pids 0,1).  It then stays running so you can
# HOT-ADD co-resident terminals to the live crossbar with
# scripts/hot-add-terminal.sh from another shell.
#
#   Terminal A:  sh scripts/boot-base.sh
#   Terminal B:  sh scripts/hot-add-terminal.sh 2     # then 3, 4, ...
#
# This works because the boot is directory-driven (Phase 45f): every CPU
# self-registers at /sys/cpu/<procid>/supervisor via the directory cap in O8,
# so a CPU that connects to the crossbar later is discovered at runtime — no
# static wiring, no restart.  cpu0 is the shutdown anchor (op=2 lands there).
#
# Stop everything with Ctrl-C.  Set OROS_SOCK to override the crossbar socket
# path (default /tmp/oros.sock); hot-add-terminal.sh reads the same default.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOCK="${OROS_SOCK:-/tmp/oros.sock}"

# Headless perf (simulator-perf-proposals): run the compute CPUs' sim under PyPy
# (~12x) when available — simorisc re-execs itself when ORISC_USE_PYPY is set.
# Overridable (ORISC_USE_PYPY=0 forces CPython); graceful if pypy3 is absent.
# Devices (oriscbar/oriscdir/hostfsd) stay on python3; a hot-added Tk terminal
# also runs under PyPy (it ships tkinter, so the WM is accelerated too).
export ORISC_USE_PYPY="${ORISC_USE_PYPY:-1}"

# Shell banner (today minus 40 years), same conceit as boot.sh, so a hot-added
# terminal's shell announces the right alternate-history date.
if date -v -40y +"%Y" >/dev/null 2>&1; then
    PAST=$(date -v -40y +"%b %e %Y %-l:%M %p" | tr -s ' ')
else
    PAST=$(date -d "40 years ago" +"%b %-d %Y %-l:%M %p")
fi
touch ouroboros/shell.c
make -s shell SHELL_BUILD_BANNER="\"Object RISC Shell ($PAST)\""
make -s all

# The co-resident chain spawns its system images from /programs.
cp -f build/supervisor.orx build/oriscwm.orx build/programs/
if [ ! -L "$ROOT/programs" ]; then rm -rf "$ROOT/programs"; ln -s build/programs "$ROOT/programs"; fi

# O5..O10 service slots: 3 null pads, O8 = directory (18=1@9), 2 null pads.
SVC="--service 0=0@0 --service 0=0@0 --service 0=0@0 --service 18=1@9 --service 0=0@0 --service 0=0@0"

PIDS=""
cleanup() {
    # Kill our own daemons + compute CPUs, plus any terminals hot-added onto
    # this crossbar (they connect to $SOCK).
    [ -n "$PIDS" ] && kill $PIDS 2>/dev/null || true
    pkill -f "tools/sim/simorisc --connect $SOCK" 2>/dev/null || true
    rm -f "$SOCK"
}
trap cleanup EXIT INT TERM

rm -f "$SOCK"
python3 tools/sim/oriscbar --socket "$SOCK" >/tmp/oros-bar.out 2>&1 & PIDS="$PIDS $!"
for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 \
    --config tools/devices/oriscdir.default.conf >/tmp/oros-dir.out 2>&1 & PIDS="$PIDS $!"
for _ in $(seq 100); do grep -q "oriscdir READY" /tmp/oros-dir.out 2>/dev/null && break; sleep 0.05; done
python3 tools/devices/hostfsd --directory-pid 18 --instance 0 \
    --socket "$SOCK" --pid 17 --root "$ROOT" >/tmp/oros-hf.out 2>&1 & PIDS="$PIDS $!"
for _ in $(seq 100); do grep -q "hostfsd READY" /tmp/oros-hf.out 2>/dev/null && break; sleep 0.05; done
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 $SVC \
    "$ROOT/build/supervisor.orx" >/tmp/oros-cpu0.out 2>&1 & PIDS="$PIDS $!"
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 $SVC \
    "$ROOT/build/supervisor.orx" >/tmp/oros-cpu1.out 2>&1 & PIDS="$PIDS $!"

echo "=================================================================="
echo " Base up on $SOCK"
echo "   crossbar + directory(pid 18) + hostfsd(pid 17) + compute pids 0,1"
echo "   logs: /tmp/oros-{bar,dir,hf,cpu0,cpu1}.out"
echo
echo " Hot-add a co-resident terminal (from another shell):"
echo "   sh scripts/hot-add-terminal.sh 2      # then 3, 4, ..."
echo
echo " Ctrl-C here to tear everything down (crossbar + CPUs + terminals)."
echo "=================================================================="
wait

#!/bin/sh
# hot-add-terminal.sh — attach a co-resident terminal to a running crossbar.
#
# Connects a fresh CPU (termfw.orx -> supervisor -> sysinit -> WM, all co-
# resident) to the crossbar started by scripts/boot-base.sh and opens a Tk
# window.  The terminal harvests the directory cap from O8 (service 18=1@9),
# registers itself at /sys/cpu/<pid>/supervisor and /sys/wm/<pid>/0, and
# launches apps from its right-click menu — local or round-robined onto the
# base's compute CPUs.  No restart of the base required.
#
#   sh scripts/hot-add-terminal.sh [pid]
#
# pid defaults to 2 (the base uses 0,1 for compute).  Use a FRESH pid for each
# terminal you attach: 2, 3, 4, ...  Set OROS_NODISPLAY=1 for a headless attach,
# OROS_SOCK to match a non-default boot-base socket.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOCK="${OROS_SOCK:-/tmp/oros.sock}"
PID="${1:-2}"

if [ ! -S "$SOCK" ]; then
    echo "hot-add-terminal: no crossbar at $SOCK — run scripts/boot-base.sh first" >&2
    exit 1
fi
if [ ! -f "$ROOT/build/termfw.orx" ]; then
    echo "hot-add-terminal: build/termfw.orx missing — boot-base.sh builds it" >&2
    exit 1
fi

# Same O5..O10 layout as the compute CPUs: O8 = directory (18=1@9), rest null.
SVC="--service 0=0@0 --service 0=0@0 --service 0=0@0 --service 18=1@9 --service 0=0@0 --service 0=0@0"
DISPLAY_OPT="--display tk"
[ -n "${OROS_NODISPLAY:-}" ] && DISPLAY_OPT=""

echo "hot-add-terminal: attaching co-resident terminal at pid $PID on $SOCK ..."
exec python3 tools/sim/simorisc --connect "$SOCK" --pid "$PID" $SVC $DISPLAY_OPT \
    "$ROOT/build/termfw.orx"

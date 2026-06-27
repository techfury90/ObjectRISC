#!/bin/sh
# boot-multi.sh — boot a multi-CPU Ouroboros system on the co-resident terminal
# path: 2 headless compute supervisors (pids 0,1) + 2 co-resident terminals
# (pids 2,3).  There is NO leader/worker split — every supervisor is a peer;
# CPUs with a display launch their own WM, compute CPUs idle for relayed
# spawns, and the first CPU to exit (a terminal "Shut Down") tears the group
# down.  Terminals are deliberately NOT the low PIDs.
#
# Two Tk windows by default; set OROS_NODISPLAY=1 for a headless run.
# The single-terminal default boot is scripts/boot.sh.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Same alternate-history shell banner as boot.sh (today minus 40 years).
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
if [ ! -L "$ROOT/programs" ]; then
    rm -rf "$ROOT/programs"
    ln -s build/programs "$ROOT/programs"
fi

# O5..O10 service slots: 3 null pads, O8 = directory (18=1@9), 2 null pads.
SVC="service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,service=0=0@0,service=0=0@0"
DISPLAY_OPT=",display=tk"
[ -n "${OROS_NODISPLAY:-}" ] && DISPLAY_OPT=""

exec python3 tools/oriscrun \
    --directory pid=18 \
    --hostfsd "pid=17,instance=0,root=$ROOT" \
    --cpu "pid=0:program=$ROOT/build/supervisor.orx,$SVC" \
    --cpu "pid=1:program=$ROOT/build/supervisor.orx,$SVC" \
    --cpu "pid=2:program=$ROOT/build/termfw.orx,$SVC$DISPLAY_OPT" \
    --cpu "pid=3:program=$ROOT/build/termfw.orx,$SVC$DISPLAY_OPT" \
    --leader-timeout 600

#!/bin/sh
# test_multiterminal_boot.sh — the peer-supervisor model (no leader/worker).
#
# Boots, via oriscrun, 2 headless compute supervisors (pids 0,1) + 2 co-resident
# terminals (pids 2,3) on the termfw boot path.  Asserts:
#   - every supervisor announces the uniform "supervisor: booting" banner —
#     no "booting (leader)" / "booting (worker)" split survives;
#   - BOTH terminals bring up their own WM at /sys/wm/<pid>/0 — i.e. the WM
#     launch keys off having a display, not off being procid 0, and terminals
#     are NOT forced to the low PIDs;
#   - killing any one CPU tears the whole group down via oriscrun's
#     any-CPU-exit watchdog (there is no designated leader to wait on).
#
# Replaces the obsolete test_supervisor_multicpu.sh, which wired a remote
# console into O5 (tripping the co-resident detector) and asserted the old
# leader/worker banners.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
make -s all >/dev/null
# The co-resident chain spawns its system images from /programs.
cp -f build/supervisor.orx build/oriscwm.orx build/programs/

TMP=$(mktemp -d)
ORPID=""
cleanup() {
    [ -n "$ORPID" ] && kill "$ORPID" 2>/dev/null || true
    pkill -f "tools/sim/simorisc --connect" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT
LOG="$TMP/run.log"

SVC="service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,service=0=0@0,service=0=0@0"
python3 tools/oriscrun \
    --directory pid=18 \
    --hostfsd "pid=17,instance=0,root=$ROOT" \
    --cpu "pid=0:program=$ROOT/build/supervisor.orx,$SVC" \
    --cpu "pid=1:program=$ROOT/build/supervisor.orx,$SVC" \
    --cpu "pid=2:program=$ROOT/build/termfw.orx,$SVC" \
    --cpu "pid=3:program=$ROOT/build/termfw.orx,$SVC" \
    --leader-timeout 90 > "$LOG" 2>&1 &
ORPID=$!

# Wait (bounded) for BOTH terminals' WMs to come up.
ok=0
for _ in $(seq 90); do
    if grep -q "\[cpu2\] oriscwm: ready" "$LOG" 2>/dev/null \
    && grep -q "\[cpu3\] oriscwm: ready" "$LOG" 2>/dev/null; then ok=1; break; fi
    kill -0 "$ORPID" 2>/dev/null || break   # oriscrun died early
    sleep 0.5
done

echo "--- boot markers ---"
grep -E "supervisor: booting|oriscwm: ready|registered at /sys/wm|trap" "$LOG" | sort -u || true

[ "$ok" = 1 ] || { echo "FAIL: both terminals' WMs did not come up" >&2; exit 1; }

# Peer model: uniform banner, no leader/worker.
grep -qE "booting \((leader|worker)\)" "$LOG" \
    && { echo "FAIL: leader/worker banner still present" >&2; exit 1; }
[ "$(grep -c "supervisor: booting" "$LOG")" -ge 4 ] \
    || { echo "FAIL: not all 4 supervisors announced booting" >&2; exit 1; }

# Both terminals self-registered distinct WMs at their own (non-zero) PIDs.
grep -q "registered at /sys/wm/2/0" "$LOG" \
    || { echo "FAIL: cpu2 WM not registered at /sys/wm/2/0" >&2; exit 1; }
grep -q "registered at /sys/wm/3/0" "$LOG" \
    || { echo "FAIL: cpu3 WM not registered at /sys/wm/3/0" >&2; exit 1; }

grep -qiE "trap|capability-violation" "$LOG" \
    && { echo "FAIL: a trap occurred during boot" >&2; exit 1; }

# Any-CPU-exit teardown: kill ONE terminal CPU and confirm oriscrun tears the
# whole group down (no leader involved).  oriscrun should exit within seconds.
pkill -f "[-][-]pid 2 " 2>/dev/null || true
torn=0
for _ in $(seq 30); do
    kill -0 "$ORPID" 2>/dev/null || { torn=1; break; }
    sleep 0.5
done
[ "$torn" = 1 ] \
    || { echo "FAIL: oriscrun did not tear the group down after a CPU exit" >&2; exit 1; }

echo "PASS"

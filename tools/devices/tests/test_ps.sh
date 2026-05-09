#!/bin/sh
# test_ps.sh — Phase 52: cross-CPU `ps` command.
#
# Architecture under test:
#   - oriscbar + hostfsd (pid 17) + oriscdir (pid 18)
#   - oriscterm pid 16 + oriscterm pid 19 (two fake terminals)
#   - simorisc CPU 0 wired to terminal 16 (services 16=*@9 etc.)
#   - simorisc CPU 1 wired to terminal 19 (services 19=*@9 etc.)
#
# term16 types: <RET>ps<RET>exit<RET>  (welcome → ps → shutdown)
# term19 types: <RET>          (acknowledges welcome, then sits on
#                                the shell prompt waiting for input)
#
# When term16's shell runs `ps`, both supervisors are live:
#   - CPU 0 has [login (blocked), shell (running)]
#   - CPU 1 has [login (blocked), shell (blocked on read_line)]
#
# (Phase 54: sysinit on the leader is EXITED by the time ps runs,
# but the supervisor's slot-table reaper has already reclaimed it,
# so it doesn't appear in the listing. That's correct semantics —
# ps shows live + recently-exited tasks; reaped slots are gone.)
#
# Asserts:
#   - ps output contains "CPU 0:" and "CPU 1:"
#   - ps output contains "shell.orx" (both CPUs have one)
#   - ps output contains "login.orx"
#   - ps output shows at least one "running" or "blocked" state
#
# Failure modes worth catching:
#   - sup_list_tasks's wire op never reached the supervisor
#     (no ps output → "CPU N:" header missing)
#   - bytes-object reply not fetched (header present but lines blank)
#   - per-task name stash broken (lines say "(unnamed)" everywhere)

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

mkdir -p "$TMP/jail/programs"

# --- shell ----------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (TEST)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/jail/programs/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

build_orx() {
    src="$1"; out="$2"
    "$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/__pp.i"
    "$CCOM" < "$TMP/__pp.i" > "$TMP/__pp.s"
    python3 tools/asm/asmorisc -r "$TMP/__pp.s" -o "$TMP/__main.oro"
    python3 tools/ld/orld -o "$out" \
        "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/__main.oro" \
        build/liborisc.ora
}
build_orx "ouroboros/programs/login.c"   "$TMP/jail/programs/login.orx"
build_orx "ouroboros/programs/sysinit.c" "$TMP/jail/programs/sysinit.orx"

# --- supervisor ------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    ouroboros/supervisor.c > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + hostfsd + oriscdir ----------------------------
SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >"$TMP/bar.out" 2>&1 &
BAR=$!
for _ in $(seq 50); do
    [ -S "$SOCK" ] && break
    sleep 0.05
done

python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 -v \
    > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do
    grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/devices/hostfsd --directory-pid 18 --instance 0 \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# --- two fake terminals --------------------------------------------
#
# term16 keystrokes: <RET>ps<RET>exit<RET>
TERM16_KEYS="\
--event key:0x10D \
--event key:p --event key:s \
--event key:0x10D \
--event key:e --event key:x --event key:i --event key:t \
--event key:0x10D"

# term19: just <RET> to dismiss banner; then sit at shell prompt.
TERM19_KEYS="\
--event key:0x10D"

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    $TERM16_KEYS \
    --linger 12.0 --delay 0.20 \
    > "$TMP/term16.out" 2>&1 &
TERM16_PID=$!

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 19 \
    --directory-pid 18 --instance 1 \
    $TERM19_KEYS \
    --linger 12.0 --delay 0.20 \
    > "$TMP/term19.out" 2>&1 &
TERM19_PID=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null \
        && grep -q "fake_terminal READY" "$TMP/term19.out" 2>/dev/null \
        && break
    sleep 0.05
done

# --- two CPUs, each wired to a different terminal --------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "19=1@9" --service "19=2@9" \
    --service "19=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM16_PID 2>/dev/null || true
wait $TERM19_PID 2>/dev/null || true
sleep 1
# term19 only sent <RET> — its CPU 1 supervisor never received op=2,
# so wait $CPU1 would block forever. Force-kill instead. cpu0
# halted naturally (term16 sent `exit`), but kill it too if it's
# somehow still alive. The CPU 0 stdout has whatever ps printed
# before halt, which is the only thing this test reads.
kill -KILL $CPU0 $CPU1 2>/dev/null || true
wait $CPU0 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"
echo "--- term16 rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out"

RENDER16=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out")

# 1) ps printed at least one CPU header.
echo "$RENDER16" | grep -q "^CPU 0:" \
    || { echo "FAIL: ps didn't print 'CPU 0:' header (op=5 reached supervisor?)" >&2; exit 1; }

# 2) The leader's task list mentions the still-live programs we
#    know are running at the moment ps runs: login + shell.
#    sysinit isn't listed because Phase 54's slot-table reaper
#    cleared its EXITED slot before ps walked the table.
echo "$RENDER16" | grep -q "shell.orx" \
    || { echo "FAIL: ps output missing shell.orx (per-task name stash broken?)" >&2; exit 1; }
echo "$RENDER16" | grep -q "login.orx" \
    || { echo "FAIL: ps output missing login.orx" >&2; exit 1; }

# 3) Cross-CPU: CPU 1 also responded.
echo "$RENDER16" | grep -q "^CPU 1:" \
    || { echo "FAIL: ps didn't print 'CPU 1:' — peer dir_walk failed?" >&2; exit 1; }

# 4) State words present (proves render_task_line built lines, not
#    just empty newlines from a broken bytes copy).
echo "$RENDER16" | grep -qE "(running|blocked|exited|runnable|new|suspended)" \
    || { echo "FAIL: no task-state words in ps output" >&2; exit 1; }

echo "PASS"

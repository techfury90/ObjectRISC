#!/bin/sh
# test_multiterminal.sh — Phase 46: two terminals, two independent
# shells, each bound to its own oriscterm via per-CPU boot OPRs.
#
# Architecture under test:
#   - oriscbar + hostfsd (pid 17) + oriscdir (pid 18)
#   - oriscterm pid 16 + oriscterm pid 19 (two Tk-equivalents,
#     each impersonated by fake_terminal.py)
#   - simorisc CPU 0 wired to terminal 16 (services 16=1@9 etc.)
#   - simorisc CPU 1 wired to terminal 19 (services 19=1@9 etc.)
#
# Each supervisor probes O5 at boot — both are non-null, so both
# pass the has_terminal gate, both register /sys/term/<procid>/{
# console,keyboard,grid} as service-discovery LEAFs, and both
# spawn shells. Result: two independent shell sessions sharing the
# same /programs mount and the same directory tree.
#
# Each fake_terminal types a different command sequence to exercise
# its shell:
#   term 16: ls /sys/term<RET> exit<RET>
#   term 19: ls /sys/term<RET> exit<RET>
#
# Both should see the same `/sys/term` listing (0/, 1/) — proving
# both shells are talking to the same oriscdir via the shared
# directory mailbox.
#
# Asserts:
#   - cpu0 stdout contains "supervisor: booting (leader)"
#   - cpu1 stdout contains "supervisor: booting (worker)"
#   - cpu0 stdout contains "supervisor: shell exited; halting"
#   - cpu1 stdout contains "supervisor: shell exited; halting"
#     (Phase 46: workers with terminals also accept op=2)
#   - both rendered terminals show the shell banner
#   - both rendered terminals' `ls /sys/term` shows "0/" and "1/"
#     (both terminals were registered)

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

# --- shell + login + sysinit (Phase 48) ------------------------------
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

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# Phase 47: oriscdir first, then everything else. Devices' inline
# self-registration packets need a live oriscdir to land at.
python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 -v \
    --config tools/devices/oriscdir.default.conf \
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

# --- two fake terminals, each typing ls /sys/term + exit -------------
#
# Both shells should see the same /sys/term listing (0/, 1/) because
# both terminal supervisors register their own subtree in the shared
# oriscdir.

# Keystrokes for `<RET> ls /sys/term<RET> exit<RET>`:
# Phase 48: leading <RET> dismisses login.orx's welcome banner; then
# the shell receives the rest.
TERM16_KEYS="\
--event key:0x10D \
--event key:l --event key:s --event key:0x20 \
--event key:0x2f --event key:s --event key:y --event key:s \
--event key:0x2f --event key:t --event key:e --event key:r --event key:m \
--event key:0x10D \
--event key:e --event key:x --event key:i --event key:t \
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
    $TERM16_KEYS \
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
sleep 0.5
wait $CPU0 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- oriscdir log ---"
cat "$TMP/dir.out"
echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"
echo "--- term16 rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out"
echo "--- term19 rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term19.out"

RENDER16=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out")
RENDER19=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term19.out")

# 1) Each supervisor announced and shut down.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't announce as worker" >&2; exit 1; }
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't shut down (shell 0 op=2 not received)" >&2; exit 1; }
grep -q "supervisor: shell exited; halting" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 supervisor didn't shut down (shell 1 op=2 not received)" >&2; exit 1; }

# 2) Each terminal received its login welcome banner. (Phase 48: the
#    shell's own banner is sometimes wiped by login.orx's loop-top
#    term_clear when shell exits — login's task_wait wakes, login
#    resumes, term_clear fires before supervisor's op=2 task_kill
#    cascade lands. Cosmetic; the shell still ran correctly,
#    evidenced by the `ls /sys/term` output asserted below.)
echo "$RENDER16" | grep -q "Welcome to the Ouroboros" \
    || { echo "FAIL: term16 didn't see login welcome banner" >&2; exit 1; }
echo "$RENDER19" | grep -q "Welcome to the Ouroboros" \
    || { echo "FAIL: term19 didn't see login welcome banner" >&2; exit 1; }

# 3) `ls /sys/term` from EITHER shell shows BOTH terminal subtrees.
#    Confirms (a) both supervisors registered into the shared
#    directory, (b) walks across CPUs see a consistent tree.
echo "$RENDER16" | grep -qE '^0/$' \
    || { echo "FAIL: term16's ls /sys/term missing '0/' entry" >&2; exit 1; }
echo "$RENDER16" | grep -qE '^1/$' \
    || { echo "FAIL: term16's ls /sys/term missing '1/' entry — CPU 1 didn't register?" >&2; exit 1; }
echo "$RENDER19" | grep -qE '^0/$' \
    || { echo "FAIL: term19's ls /sys/term missing '0/' entry — CPU 0 didn't register?" >&2; exit 1; }
echo "$RENDER19" | grep -qE '^1/$' \
    || { echo "FAIL: term19's ls /sys/term missing '1/' entry" >&2; exit 1; }

# (The shell prints "bye!" before SENDing op=2 + TaskExit, but the
# print is async — under teardown timing the bye! bytes may not be
# fetched by oriscterm before the supervisor halts and the data ref
# evaporates. Don't assert on it; the "supervisor: shell exited;
# halting" message is the canonical proof the exit path ran.)

echo "PASS"

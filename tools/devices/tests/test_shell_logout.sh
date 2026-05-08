#!/bin/sh
# test_shell_logout.sh — Phase 48: `logout` ends a shell session
# without halting the supervisor. login.orx's task_wait wakes,
# its loop redraws the welcome banner, and the next Enter starts
# a fresh shell session — same as the very first one at boot.
#
# Architecture under test:
#   - oriscbar + hostfsd + oriscdir (single CPU, single terminal)
#   - one supervisor on procid 0, leader, terminal-equipped
#   - login.orx as first user task (Phase 48)
#   - shell.orx spawned by login on user's first <RET>
#
# Keystroke script:
#   <RET>            dismiss boot welcome → login spawns shell #1
#   logout<RET>      shell #1 exits cleanly; login redraws banner
#   <RET>            dismiss post-logout welcome → spawn shell #2
#   exit<RET>        shell #2 halts the supervisor — clean shutdown
#                    so the test doesn't have to forcibly kill the
#                    simulator process to terminate.
#
# Asserts (everything verified from cpu0 stdout, since login's
# term_clear at the top of each iteration wipes earlier on-screen
# content before fake_terminal can render it):
#   - cpu0 stdout contains "supervisor: booting (leader)"
#   - cpu0 stdout contains "login: shell exited cleanly" — the
#     diagnostic marker that orx_unload returned (logout path —
#     vs. supervisor's op=2 task_kill cascade, which bypasses
#     login's loop entirely).
#   - cpu0 stdout contains "login: top of loop" AT LEAST TWICE
#     (once at boot, once after orx_unload returned).
#   - cpu0 stdout contains "supervisor: shell exited; halting"
#     (clean shutdown via shell #2's `exit`).

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

# --- shell + login + sysinit -----------------------------------------
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

python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 \
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

# Keystrokes: <RET> logout<RET> <RET> exit<RET>
#   <RET>            dismiss boot welcome → spawn shell #1
#   logout<RET>      shell #1 exits cleanly via `return 0`
#   <RET>            dismiss post-logout welcome → spawn shell #2
#   exit<RET>        shell #2 halts the supervisor (sup_shutdown
#                    op=2), supervisor halts, CPU exits — no
#                    kill -KILL fallback needed in cleanup.
TERM16_KEYS="\
--event key:0x10D \
--event key:l --event key:o --event key:g --event key:o --event key:u --event key:t \
--event key:0x10D \
--event key:0x10D \
--event key:e --event key:x --event key:i --event key:t \
--event key:0x10D"

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    $TERM16_KEYS \
    --linger 5.0 --delay 0.15 \
    > "$TMP/term16.out" 2>&1 &
TERM16_PID=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null && break
    sleep 0.05
done

# --- one CPU, one terminal -------------------------------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

wait $TERM16_PID 2>/dev/null || true
sleep 0.3
wait $CPU0 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- term16 rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out"

RENDER16=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term16.out")

# 1) Supervisor came up and shut down cleanly.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't shut down" >&2; exit 1; }

# 2) Login's task_wait properly returned after `logout` — the
#    diagnostic print fires right after orx_unload completes.
grep -q "login: shell exited cleanly" "$TMP/cpu0.out" \
    || { echo "FAIL: login.orx_unload never returned " \
              "(task_wait stuck on shell?)" >&2; exit 1; }

# 3) Login looped back to the top, which means the welcome
#    banner was redrawn for shell #2's session.
TOP_COUNT=$(grep -c "login: top of loop" "$TMP/cpu0.out" || true)
if [ "$TOP_COUNT" -lt 2 ]; then
    echo "FAIL: login looped $TOP_COUNT time(s); expected at " \
         "least 2 (boot + post-logout)" >&2
    exit 1
fi

echo "PASS"

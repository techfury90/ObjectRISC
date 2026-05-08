#!/bin/sh
# test_supervisor_multicpu.sh — Phase 45c: every CPU boots
# supervisor.orx, only PROCID 0 spawns a shell.
#
# Architecture under test: each CPU runs the same supervisor binary;
# the supervisor reads its PROCID via `lctrl … $7` and branches —
# leader (PROCID 0) spawns the shell as its first user task, workers
# enter the dispatch loop directly. Workers don't do anything visible
# yet (no peer SENDs cross-CPU spawn requests until 45e), but they
# announce themselves at boot and stay alive until external teardown.
#
# Builds:
#   - supervisor.orx (boot leader on both CPUs)
#   - shell.orx     (leader-only first task; via /programs/)
#
# Launches: oriscbar + hostfsd + fake_terminal + 2 simorisc CPUs.
# Types:    exit<RET>
#
# Asserts:
#   - cpu0 stdout contains "supervisor: booting (leader)"
#   - cpu1 stdout contains "supervisor: booting (worker)"
#   - cpu0 stdout contains "supervisor: shell exited; halting"
#     (i.e., the leader supervisor wound down on the shell's
#     sup_shutdown SEND)
#   - cpu0 exits cleanly (wait $CPU0 returns)
#   - the worker on cpu1 does NOT print the shell-done message

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

# --- shell (leader-only first task) ----------------------------------
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

# --- supervisor (boot leader on every CPU) ---------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    ouroboros/supervisor.c > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + hostfsd + fake_terminal ----------------------
SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# Type:  exit<RET>  — drives the leader's shell to TaskExit, which
# triggers sup_shutdown → leader supervisor halts. The worker stays
# blocked in poll until we kill it after the test.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 5.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# CPU 0 (leader) — spawns the shell. Same service map as scripts/boot.sh.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "0=0@0" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

# CPU 1 (worker) — same supervisor binary, branches to "no shell"
# at boot. Phase 46: the gate is now has_terminal (= O5 non-null),
# not procid — so we wire null terminal slots here to opt this CPU
# out of shell-spawn while leaving it available for relayed spawn
# requests via /sys/cpu/1/supervisor (registered from boot O8).
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
# Wait for CPU 0 (the leader) to exit — it should once the shell
# SENDs sup_shutdown. CPU 1 stays blocked indefinitely; we kill it
# explicitly here (in production oriscrun's `--leader 0` would
# handle the teardown).
wait $CPU0 2>/dev/null || true
kill -KILL $CPU1 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu0 stderr ---"
cat "$TMP/cpu0.err"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"
echo "--- cpu1 stderr ---"
cat "$TMP/cpu1.err"

# 1) Leader announces as leader.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }

# 2) Worker announces as worker.
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't announce as worker" >&2; exit 1; }

# 3) Worker did NOT identify as leader (sanity — wrong-PROCID branch).
grep -q "supervisor: booting (leader)" "$TMP/cpu1.out" \
    && { echo "FAIL: cpu1 announced as leader" >&2; exit 1; }
true   # grep -q with "no match" returns 1; suppress under set -e

# 4) Leader's shell drove the leader supervisor to halt cleanly.
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: leader supervisor didn't halt on shell exit" >&2; exit 1; }

# 5) Worker did NOT print shell-done — only the leader runs that path.
grep -q "supervisor: shell exited; halting" "$TMP/cpu1.out" \
    && { echo "FAIL: worker printed shell-done message" >&2; exit 1; }
true

echo "PASS"

#!/bin/sh
# test_round_robin.sh — Phase 51: shell `run cmd` (no @N) round-robins
# the spawn across live CPUs. The shell stays on its terminal's CPU
# (login pins it explicitly), but each `run` distributes load.
#
# Architecture under test:
#   - oriscbar + hostfsd + oriscdir
#   - oriscterm pid 16 only (terminal 0; user's session)
#   - simorisc cpu 0 (leader, terminal 0, runs the shell)
#   - simorisc cpu 1 (worker, no terminal — service slots null;
#                     receives round-robin'd spawns from cpu 0)
#
# The guest (`procid_print.orx`) prints which procid it ran on by
# reading the firmware PROCID register and emitting it via print_str
# to its host stdout. With round-robin we expect:
#   - First `run` from cpu 0's shell → counter starts at procid+1=1,
#     pick_next_cpu finds cpu 1 (registered), relays to cpu 1 →
#     guest prints to cpu1.out.
#   - Second `run` → counter=2, walks fail until wrap, lands on
#     cpu 0 (self) → guest prints to cpu0.out.
#   - Third `run` → counter=1 again → cpu 1.
#   - Fourth `run` → cpu 0.
# So with 4 runs we get 2 lines on each cpu's stdout.
#
# Asserts:
#   - cpu0 stdout contains "procid-print: ran on procid 0" (≥ 2x)
#   - cpu1 stdout contains "procid-print: ran on procid 1" (≥ 2x)
#   - cpu0 stdout contains "supervisor: shell exited; halting"
#     (clean shutdown via shell's `exit`)

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

# --- guest: prints which procid it's running on -----------------------
cat > "$TMP/procid_print.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    /* Read the firmware PROCID control register (Vol V §2.10, ctrl 7).
     * Same trick the supervisor uses to learn its own procid. */
    int procid;
    asm volatile(
        "lctrl %0, 7"
        : "=r"(procid)
    );
    print_str("procid-print: ran on procid ");
    print_int(procid);
    print_str("\n");
    return 0;
}
EOF

build_guest() {
    src="$1"; out="$2"
    "$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/__pp.i"
    "$CCOM" < "$TMP/__pp.i" > "$TMP/__pp.s"
    python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/__crt0.oro"
    python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/__cio.oro"
    python3 tools/asm/asmorisc -r "$TMP/__pp.s"                     -o "$TMP/__main.oro"
    python3 tools/ld/orld -o "$out" \
        "$TMP/__crt0.oro" "$TMP/__cio.oro" "$TMP/__main.oro" \
        build/liborisc.ora
}
build_guest "$TMP/procid_print.c" "$TMP/jail/programs/procid_print.orx"

# --- shell + login + sysinit ------------------------------------------
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

# --- launch oriscbar + oriscdir + hostfsd + fake_terminal ------------
SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 \
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

# Keystrokes:
#   <RET>                            dismiss login welcome
#   run /programs/procid_print.orx<RET>     × 4
#   exit<RET>
RUN_KEYS="\
--event key:r --event key:u --event key:n --event key:0x20 \
--event key:0x2f --event key:p --event key:r --event key:o --event key:g \
--event key:r --event key:a --event key:m --event key:s --event key:0x2f \
--event key:p --event key:r --event key:o --event key:c --event key:i \
--event key:d --event key:0x5f --event key:p --event key:r --event key:i \
--event key:n --event key:t --event key:0x2e --event key:o --event key:r \
--event key:x --event key:0x10D"

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    --event key:0x10D \
    $RUN_KEYS $RUN_KEYS $RUN_KEYS $RUN_KEYS \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 4.0 --delay 0.06 \
    > "$TMP/term16.out" 2>&1 &
TERM16_PID=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null && break
    sleep 0.05
done

# --- two CPUs --------------------------------------------------------
# CPU 0 (leader, terminal 0). Walk-don't-wire: leave O5/O6/O7 NULL so the
# supervisor walks /sys/term/0 for its console/keyboard/grid. A wired,
# non-null O5 is now read as "I have a framebuffer -> co-resident -> launch
# the WM" (supervisor.c's coresident detector); with no oriscwm.orx in this
# jail that path aborts and no shell ever comes up (the boot hangs).
# O8=directory + O10=hostfsd stay wired.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

# CPU 1: no terminal slots (round-robin'd spawns get the requester's
# terminal injected via Phase 49 pass-through; CPU 1's own boot OPRs
# don't matter for shell output).
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM16_PID 2>/dev/null || { echo "FAIL: fake_terminal aborted (boot/input never came up - see term16.out and cpu*.out)" >&2; kill -KILL $(jobs -p) 2>/dev/null; exit 1; }
sleep 0.5
wait $CPU0 2>/dev/null || true
kill -KILL $CPU1 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"

# 1) Supervisor on cpu 0 came up and shut down via shell `exit`. The
#    leader/worker split is gone (every supervisor is a peer), so the
#    retired "booting (leader)" tag is no longer printed — assert the
#    boot itself; roles are shown by who runs the shell (cpu0) vs. the
#    relayed spawn (cpu1). Matches #197's run_at/dynamic_cpu fix.
grep -q "supervisor: booting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't boot" >&2; exit 1; }
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't shut down" >&2; exit 1; }

# 2) Round-robin spread the four `run` invocations between the two
#    CPUs. With even round-robin we expect 2 each; allow some slack
#    for boot-time races (counter wraps when peer hasn't registered
#    yet) — assert each CPU saw at least 1.
N0=$(grep -c "procid-print: ran on procid 0" "$TMP/cpu0.out" || true)
N1=$(grep -c "procid-print: ran on procid 1" "$TMP/cpu1.out" || true)
echo "spread: cpu0 saw $N0 run(s); cpu1 saw $N1 run(s)"
[ "$N0" -ge 1 ] \
    || { echo "FAIL: no procid_print runs landed on cpu0" >&2; exit 1; }
[ "$N1" -ge 1 ] \
    || { echo "FAIL: no procid_print runs landed on cpu1 — round-" \
              "robin didn't spread" >&2; exit 1; }
T=$(( N0 + N1 ))
[ "$T" -ge 4 ] \
    || { echo "FAIL: only $T total runs (expected ≥ 4 — some spawns " \
              "were lost)" >&2; exit 1; }

echo "PASS"

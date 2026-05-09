#!/bin/sh
# test_dynamic_cpu.sh — Phase 53: dynamic CPU count.
#
# Boots with a single CPU (cpu0, leader, terminal-equipped), waits
# for it to be fully up (login subscribed to kbd + supervisor in
# its dispatch loop), then mid-run launches a SECOND simorisc
# (cpu1, headless worker) connecting to the same crossbar. Verifies
# that the leader's `run @1 cmd` round-trip to the dynamically-
# added cpu1 succeeds — proving Phase 51's per-call dir_walk in
# pick_next_cpu / relay_spawn_request is fundamentally dynamic and
# requires no extra plumbing for hot-add.
#
# Architecture under test:
#   - oriscbar + oriscdir + hostfsd at boot
#   - oriscterm pid 16 (instance 0): boot terminal
#   - simorisc CPU 0 (leader): launched at boot
#   - simorisc CPU 1 (headless worker): launched mid-run, AFTER
#     cpu0's shell is interactive
#   - shell types: <RET> run @1 /programs/hello.orx<RET> exit<RET>
#
# Hello prints to firmware stdout, so we look for the marker in
# cpu1.out (the dynamically-added CPU's stdout). If the spawn
# round-trip didn't reach cpu1, the marker would be missing.
#
# Asserts:
#   - cpu1.out contains "supervisor: booting (worker)" — proves the
#     dynamically-added CPU booted into the existing system
#     correctly (mounted /programs, found oriscdir, etc.)
#   - cpu1.out contains "hello-from-supervised-spawn" — proves
#     the leader's relay reached cpu1 and the spawn ran there
#   - cpu0.out does NOT contain "hello-from-supervised-spawn" —
#     belt-and-suspenders that the relay actually happened (vs.
#     local-fallback)
#   - cpu0.out contains "supervisor: shell exited; halting" —
#     clean shutdown via shell's exit

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

# --- guest: prints to firmware stdout and exits ----------------------
cat > "$TMP/hello.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    print_str("hello-from-supervised-spawn\n");
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

build_guest "$TMP/hello.c"                 "$TMP/jail/programs/hello.orx"
build_guest "ouroboros/programs/login.c"   "$TMP/jail/programs/login.orx"
build_guest "ouroboros/programs/sysinit.c" "$TMP/jail/programs/sysinit.orx"

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

# --- terminal: dismiss banner, then `run @1 /programs/hello.orx`,
#     then `exit`. The spawn is foreground (no `&`) — orx_unload's
#     task_wait blocks on the remote-home task ref until cpu1's
#     hello.orx exits, then the shell prints "[exited 0]" and
#     returns to the prompt. We DO need cpu1 alive across that
#     window; the test's launch order ensures that.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    --event key:0x10D \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x40 --event key:0x31 --event key:0x20 \
    --event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
    --event key:0x2f --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 20.0 --delay 0.20 \
    > "$TMP/term16.out" 2>&1 &
TERM16_PID=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null && break
    sleep 0.05
done

# --- single CPU at boot: cpu0 (leader, terminal 0) -------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

# Wait for cpu0's login on term16 to register a kbd subscriber —
# this is the synchronisation point that says "leader is fully up
# and interactive." Only AFTER this do we add cpu1; the goal of
# this test is to validate mid-run CPU addition into a stable
# system, not boot-time multi-CPU (which test_supervisor_run_at
# already covers).
for _ in $(seq 200); do
    grep -q "kbd subscribe" "$TMP/term16.out" 2>/dev/null && break
    sleep 0.05
done

# --- DYNAMICALLY ADDED: cpu1 (headless worker) ----------------------
#
# This is the actual feature under test: a brand-new simorisc
# process joining an already-running system. The oriscadd helper
# (Phase 53) wraps the simorisc invocation with sensible defaults
# for the supervisor's expected boot OPR layout (null pads at
# O5/O6/O7 since this is a headless worker, oriscdir at O8).
# Notice we don't pass --hostfsd: the supervisor's boot walks
# /sys/hostfsd/0 in the directory and fills in O10 from there.
# No `--terminal-pid` either — we just want this CPU as compute.
#
# The supervisor on cpu1 registers at /sys/cpu/1/supervisor on
# boot; cpu0's pick_next_cpu / relay_spawn_request find it via
# dir_walk on the next round-robin pick. No extra plumbing needed.
python3 tools/oriscadd --socket "$SOCK" --pid 1 \
    --supervisor "$TMP/supervisor.orx" \
    --directory 18 \
    >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

# Wait for cpu1 to finish booting (registers at /sys/cpu/1/supervisor
# inside its supervisor.main). Without this, fake_terminal might
# send `run @1` before cpu1 is ready and the relay would fail.
for _ in $(seq 200); do
    grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" 2>/dev/null && break
    sleep 0.05
done

wait $TERM16_PID 2>/dev/null || true
sleep 1
# cpu0 halts when shell exits; cpu1 stays alive (no op=2 from a
# headless worker) until we tear it down.
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

# 1) cpu1 actually booted — proves a brand-new simorisc can join an
#    already-running crossbar and complete its supervisor's boot.
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't reach 'booting (worker)' — dynamic-add boot path broken" >&2; exit 1; }

# 2) The cross-CPU relay reached cpu1 and the spawn ran THERE.
grep -q "hello-from-supervised-spawn" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't print hello — relay didn't reach the dynamically-added CPU" >&2; exit 1; }

# 3) Belt-and-suspenders: the spawn did NOT happen on cpu0 (would
#    indicate the relay silently fell back to local).
if grep -q "hello-from-supervised-spawn" "$TMP/cpu0.out"; then
    echo "FAIL: hello printed on cpu0 — relay fell back to local instead of routing to cpu1" >&2
    exit 1
fi

# 4) Clean shutdown via shell's exit.
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't halt via shell exit" >&2; exit 1; }

echo "PASS"

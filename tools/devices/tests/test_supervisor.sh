#!/bin/sh
# test_supervisor.sh — end-to-end test of the Phase 45b supervisor.
#
# Phase 45b architecture: supervisor.orx is CPU 0's boot leader. It
# allocates a spawn-service mailbox, derives a sub-cap into
# ORX_SLOT_CHILD_O8 so every TaskCreate it does injects the cap
# into the child's O8, then spawns the shell as its first user task.
# The shell's `run` (via cmd_run → sup_spawn) SENDs spawn requests
# to the supervisor's mailbox; the supervisor handles them and
# replies with the new task ref. On `exit`, the shell SENDs op=2
# (sup_shutdown) so the supervisor can wind down without polling.
#
# Builds:
#   - supervisor.orx (CPU 0 leader)
#   - shell.orx     (in /programs/, loaded by supervisor at boot)
#   - hello.orx     (in /programs/, loaded by shell via sup_spawn)
#
# Launches: oriscbar + hostfsd + fake_terminal + supervisor CPU.
# Types:    run /programs/hello.orx<RET>
#           exit<RET>
#
# Asserts:
#   - supervisor stdout contains "hello-from-supervised-spawn"
#     (proves the spawn-RPC round-trip worked)
#   - supervisor stdout contains "supervisor: shell exited; halting"
#     (proves the shutdown-SEND wakes the supervisor)
#   - supervisor exits cleanly (CPU0 wait returns, no kill needed)

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

build_guest "$TMP/hello.c"                "$TMP/jail/programs/hello.orx"
# Phase 48: each terminal-equipped supervisor spawns login.orx as its
# first user task; login.orx waits for Enter and then sup_spawns the
# shell. The leader additionally spawns sysinit.orx (one-shot) for
# the /programs MOUNT setup that used to live inline in supervisor.c.
build_guest "ouroboros/programs/login.c"   "$TMP/jail/programs/login.orx"
build_guest "ouroboros/programs/sysinit.c" "$TMP/jail/programs/sysinit.orx"

# --- shell (lives in /programs/ inside the jail) ---------------------
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

# --- supervisor (CPU 0 boot leader, NOT in the jail) ----------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    ouroboros/supervisor.c > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + oriscdir + hostfsd + fake_terminal -----------
SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# Phase 47: oriscdir must be live before the self-registering devices
# (hostfsd, fake_terminal) SEND their DIR_OP_REG_INLINE, and before the
# supervisor walks /sys/term/0 for its console (walk-don't-wire, below).
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

# Type:  <RET>                              (Phase 48: dismiss
#                                            login.orx's welcome
#                                            banner — Enter spawns
#                                            the shell)
#        run /programs/hello.orx<RET>
#        exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    --event key:0x10D \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
    --event key:0x2f --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 12.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# Supervisor as CPU 0 leader. Walk-don't-wire (matches run_at /
# session_manager): leave O5/O6/O7 NULL so CPU 0 walks /sys/term/0 for its
# console/keyboard/grid. A wired, non-null O5 is now read as "I have a
# framebuffer -> co-resident -> launch the WM" (supervisor.c's coresident
# detector); with no oriscwm.orx in this jail that path aborts and no shell
# ever comes up. O8=directory + O10=hostfsd stay wired.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

wait $TERM_PID 2>/dev/null || { echo "FAIL: fake_terminal aborted (boot/input never came up - see term.out and cpu*.out)" >&2; kill -KILL $(jobs -p) 2>/dev/null; exit 1; }
sleep 0.5
wait $CPU0 2>/dev/null || true
for p in $HF $DIR $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $DIR $BAR; do wait $p 2>/dev/null || true; done

echo "--- hostfsd log ---"
cat "$TMP/hf.out"
echo "--- supervisor stdout ---"
cat "$TMP/cpu0.out"
echo "--- supervisor stderr ---"
cat "$TMP/cpu0.err"

sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out" \
    > "$TMP/rendered.txt"
echo "--- rendered ---"
cat "$TMP/rendered.txt"

# 1) Spawn round-trip: hello printed via supervisor's CPU stdout.
grep -q "hello-from-supervised-spawn" "$TMP/cpu0.out" \
    || { echo "FAIL: hello-from-supervised-spawn not in supervisor stdout" >&2; exit 1; }

# 2) Shutdown notification: shell's sup_shutdown() reached supervisor.
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: supervisor didn't print shell-exited message" >&2; exit 1; }

echo "PASS"

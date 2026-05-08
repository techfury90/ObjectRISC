#!/bin/sh
# test_supervisor_run_at.sh — Phase 45e: cross-CPU spawn via shell
# `run @N command` syntax.
#
# Architecture under test: every CPU boots supervisor.orx (Phase 45c),
# spawn mailbox is at deterministic descriptor idx 5 (Phase 45e), boot
# OPRs include each CPU's peer-supervisor sub-cap at O8, supervisor
# harvests it into PEER_SUP_SLOT. The shell parses `run @N path` and
# packs N into R6 of the op=1 SEND. The local supervisor receives,
# sees target_pid != self.procid, and relays to PEER_SUP_SLOT. The
# peer reads bytes via ObjFetchBytes (remote OBJ_READ_REQ to the
# original requester's bytes object), spawns locally, replies directly
# to the shell's reply mailbox. The returned task ref has home = peer
# CPU; orx_unload's task_wait routes via Phase 45d's remote TaskWait.
#
# Builds:
#   - supervisor.orx (boot leader on every CPU)
#   - shell.orx     (leader-only first task)
#   - hello.orx     (printed identifier includes 'run @N')
#
# Launches: oriscbar + hostfsd + fake_terminal + 2 simorisc CPUs.
# Types:    run @1 /programs/hello.orx<RET>  exit<RET>
#
# Asserts:
#   - cpu0 stdout contains "supervisor: booting (leader)"
#   - cpu1 stdout contains "supervisor: booting (worker)"
#   - cpu1 stdout contains "hello-from-supervised-spawn"
#     (the spawn happened on CPU 1, NOT CPU 0)
#   - cpu0 stdout does NOT contain "hello-from-supervised-spawn"
#     (would indicate the relay didn't happen)
#   - cpu0 stdout contains "supervisor: shell exited; halting"

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

# --- guest --------------------------------------------------------------
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

build_guest "$TMP/hello.c" "$TMP/jail/programs/hello.orx"

# --- shell --------------------------------------------------------------
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

# --- supervisor ---------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    ouroboros/supervisor.c > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + hostfsd + fake_terminal --------------------------
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

# Type:  run @1 /programs/hello.orx<RET>  exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x40 --event key:0x31 --event key:0x20 \
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

# CPU 0 (leader) — supervisor mailbox at idx 6 (deterministic in
# socket mode: init_cpu's 4 reserved descriptors + simorisc's
# populate_self_service at idx 5 + the supervisor's allocate-first
# yields idx 6). Each --service slot maps to O5..O10 in order:
# term-cons, term-kbd, term-grid, peer-mailbox (CPU 1's), pad, hostfsd.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "1=6@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

# CPU 1 (worker) — same supervisor binary, but its O8 = CPU 0's mailbox.
python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "0=6@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
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
echo "--- rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out"

# 1) Boot announcements with the right roles.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't announce as worker" >&2; exit 1; }

# 2) The spawn happened on CPU 1 (peer), not CPU 0.
grep -q "hello-from-supervised-spawn" "$TMP/cpu1.out" \
    || { echo "FAIL: hello didn't print on cpu1 (the relay target)" >&2; exit 1; }
grep -q "hello-from-supervised-spawn" "$TMP/cpu0.out" \
    && { echo "FAIL: hello printed on cpu0; relay didn't fire" >&2; exit 1; }
true

# 3) Leader supervisor wound down on shell exit.
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: leader didn't shut down" >&2; exit 1; }

echo "PASS"

#!/bin/sh
# test_term_passthrough.sh — Phase 49: a `run @N cmd` from terminal X's
# shell should land its output on terminal X regardless of which CPU the
# `cmd` runs on. Pre-Phase 49 the spawned task inherited the receiving
# CPU's boot O5/O6/O7, sending its term_print's to the WRONG terminal
# (the one bound to procid N, not the one the user is sitting at).
# Phase 49 fixes this by carrying the requester's terminal index in the
# relay op and dir-walking /sys/term/<N>/* on the receiving supervisor
# to inject /sys/term/<requester>/{console,keyboard,grid} into the
# child's OPR file (via the same ORX_SLOT_CHILD_* swap dance the O8
# inheritance already uses).
#
# Architecture under test:
#   - oriscbar + hostfsd + oriscdir
#   - oriscterm pid 16 (terminal 0) + oriscterm pid 19 (terminal 1)
#   - simorisc cpu 0 (leader, terminal 0)
#   - simorisc cpu 1 (worker, terminal 1)
#
# Keystroke script (only terminal 0 types; terminal 1 just sits there):
#   <RET>                      dismiss boot welcome → shell #1
#   run @1 /programs/term_hello.orx<RET>
#                              relay to cpu 1; term_hello prints "hello
#                              from term-print" via term_print → with
#                              pass-through, goes to /sys/term/0/console
#                              (oriscterm 16). Without pass-through,
#                              would go to /sys/term/1/console.
#   exit<RET>                  halt the supervisor; oriscrun's --leader
#                              watchdog SIGTERMs cpu 1.
#
# Asserts:
#   - cpu1 stdout contains "supervisor: booting (worker)" (relay actually
#     reached cpu 1 — the spawn got there)
#   - terminal 0 render contains "hello from term-print" (pass-through
#     routed term_print to the requester's terminal)
#   - terminal 1 render does NOT contain "hello from term-print"
#     (the requester's terminal is the only one that should have it)

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

# --- guest: prints to its O5/O6/O7 terminal via term_print ------------
cat > "$TMP/term_hello.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    /* term_print_only_init parks the boot O2/O3/O4 saves the way
     * term_init does, but skips the keyboard subscribe — we don't
     * need keys, just a print path. The pass-through-injected O5
     * (console service) is what term_print writes to. */
    term_print_only_init();
    term_print("hello from term-print\n");
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

build_guest "$TMP/term_hello.c" "$TMP/jail/programs/term_hello.orx"

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

# --- launch oriscbar + hostfsd + oriscdir ----------------------------
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

# --- two fake terminals ----------------------------------------------
# Terminal 0: drive the test (`<RET> run @1 ...<RET> exit<RET>`).
# Terminal 1: idle (boot welcome, no keystrokes — login on cpu 1 just
# sits in term_getkey forever).
TERM16_KEYS="\
--event key:0x10D \
--event key:r --event key:u --event key:n --event key:0x20 \
--event key:0x40 --event key:0x31 --event key:0x20 \
--event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
--event key:0x2f --event key:t --event key:e --event key:r --event key:m --event key:0x5f --event key:h --event key:e --event key:l --event key:l --event key:o \
--event key:0x2e --event key:o --event key:r --event key:x \
--event key:0x10D \
--event key:e --event key:x --event key:i --event key:t \
--event key:0x10D"

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    $TERM16_KEYS \
    --linger 6.0 --delay 0.15 \
    > "$TMP/term16.out" 2>&1 &
TERM16_PID=$!

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 19 \
    --directory-pid 18 --instance 1 \
    --linger 8.0 --delay 0.15 \
    > "$TMP/term19.out" 2>&1 &
TERM19_PID=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null \
        && grep -q "fake_terminal READY" "$TMP/term19.out" 2>/dev/null \
        && break
    sleep 0.05
done

# --- two CPUs ---------------------------------------------------------
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
sleep 0.5
wait $CPU0 2>/dev/null || true
# CPU 1's worker.login never sees an `exit` (only terminal 0 types),
# so kill it explicitly. fake_terminal 19's linger expires shortly.
kill -KILL $CPU1 2>/dev/null || true
wait $CPU1 2>/dev/null || true
kill -KILL $TERM19_PID 2>/dev/null || true
wait $TERM19_PID 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

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

# 1) Both supervisors booted; cpu 0 halted via shell exit.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't announce as worker" >&2; exit 1; }
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't shut down" >&2; exit 1; }

# 2) The relayed spawn actually landed on cpu 1. term_hello prints to
#    its terminal via term_print, NOT print_str, so we can't see it on
#    cpu1.out — but we CAN see that cpu1's supervisor handled an op=1
#    (shell.exit relays op=2 through leader, so any cpu1 stdout past
#    "booting (worker)" implies activity occurred). The actual spawn
#    confirmation is the rendered output below.

# 3) Terminal 0 (the requester) shows the term_hello output. With
#    pass-through, the relayed task injected /sys/term/0/console as
#    its O5 — its term_print landed here, NOT on terminal 1.
echo "$RENDER16" | grep -q "hello from term-print" \
    || { echo "FAIL: terminal 0 didn't see 'hello from term-print' " \
              "(pass-through didn't route output to requester's terminal)" >&2
         exit 1; }

# 4) Terminal 1 (CPU 1's local terminal) does NOT show it. Without
#    pass-through it would (CPU 1's boot O5 is /sys/term/1/console);
#    with pass-through the spawn used the requester's terminal instead.
if echo "$RENDER19" | grep -q "hello from term-print"; then
    echo "FAIL: terminal 1 saw 'hello from term-print' — pass-through " \
         "didn't take effect (relay used cpu1's boot OPRs instead)" >&2
    exit 1
fi

echo "PASS"

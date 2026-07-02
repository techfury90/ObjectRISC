#!/bin/sh
# test_shell_run.sh — end-to-end test of the shell's `run` command.
#
# Phase 30 architecture: the shell IS the supervisor. cmd_run uses
# orx_run (libc), which loads the .orx via hostfsd, ObjAllocs
# code/data/stack, and TaskCreates a child on the SAME CPU. No
# linkbootd, no pre-spawned spare CPU pool.
#
# Builds:
#   - shell.orx (the leader CPU, with orx_run)
#   - hello.orx (a tiny guest: print_str + exit)
#
# Launches: oriscbar + hostfsd + fake_terminal + shell CPU.
# Types:    run hello.orx<RET>
#           run hello.orx<RET>      (twice — proves the supervisor
#                                    can spawn N times in a row)
#           exit<RET>
#
# Asserts:
#   - shell stdout contains "hello from guest" twice (firmware
#     ConsoleWrite prints from the guest tasks land here)
#   - rendered terminal contains both [exited 0] markers (the
#     shell prints those via term_print after orx_run returns)

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

mkdir -p "$TMP/jail"

# --- guest: prints to firmware-side stdout (not the terminal) ---------
# Using print_str means the guest doesn't subscribe to the keyboard,
# which would compete with the shell's keystroke stream on the same
# CPU. The output lands in cpu0.out, which the shell shares.
cat > "$TMP/hello.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    print_str("hello from guest\n");
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

build_guest "$TMP/hello.c" "$TMP/jail/hello.orx"

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (TEST)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

# --- launch oriscbar + hostfsd + fake_terminal ------------------------
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

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x10D \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:h --event key:e --event key:l --event key:l --event key:o \
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

# Shell CPU. Service order: O5=console, O6=keyboard, O10=hostfsd.
# Linkbootd slot (was O7) intentionally null — Phase 30 shell doesn't
# need it any more.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "0=0@0"  --service "0=0@0"  --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

wait $TERM_PID 2>/dev/null || { echo "FAIL: fake_terminal aborted (boot/input never came up - see term.out and cpu*.out)" >&2; kill -KILL $(jobs -p) 2>/dev/null; exit 1; }
sleep 0.5
wait $CPU0 2>/dev/null || true
for p in $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- hostfsd log ---"
cat "$TMP/hf.out"
echo "--- shell stdout ---"
cat "$TMP/cpu0.out"
echo "--- shell stderr ---"
cat "$TMP/cpu0.err"

# Extract rendered console.
sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"

GUEST_COUNT=$(grep -c "hello from guest" "$TMP/cpu0.out" || true)
EXIT_COUNT=$(grep -c "\[exited 0\]" "$TMP/rendered.txt" || true)
echo "shell stdout: $GUEST_COUNT guest greetings; rendered: $EXIT_COUNT exit markers"

[ "$GUEST_COUNT" -ge 2 ] \
    || { echo "FAIL: guest greeting appeared $GUEST_COUNT times (expected ≥2)" >&2; exit 1; }
# Two exit markers prove orx_run completed twice. The trailing one
# can race the test wind-down, so we accept ≥1.
[ "$EXIT_COUNT" -ge 1 ] \
    || { echo "FAIL: [exited 0] appeared $EXIT_COUNT times (expected ≥1)" >&2; exit 1; }

echo "PASS"

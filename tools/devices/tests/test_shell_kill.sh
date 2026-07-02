#!/bin/sh
# test_shell_kill.sh — verify the shell's `kill <task>` command.
#
# Sequence:
#     run spinner.orx &<RET>    ; bg a 5M-iter CPU-bound spinner
#     kill 0<RET>                ; externally terminate it
#     exit<RET>
#
# Asserts the rendered terminal contains:
#   - "[bg task 0]"        (cmd_run with & confirmed the spawn)
#   - "[task 0 done 137]"  (auto-reaper saw it EXITED with the
#                          shell's KILL_EXIT_CODE = 137)
#
# Without #0x00A the kill command would have nothing to call and
# the spinner would still be runnable. (The shell's "bye!" on exit
# races TaskExit and isn't reliable — see test_shell.sh's note.)

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

# --- guest: long CPU-bound spinner ------------------------------------
cat > "$TMP/spinner.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
	unsigned int i;
	unsigned int sum;

	term_print_only_init();
	sum = 0;
	for (i = 0; i < 5000000; i++) {
		sum = sum + i;
	}
	term_print("spinner: done\n");
	return sum & 1;
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

build_guest "$TMP/spinner.c" "$TMP/jail/spinner.orx"

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (KILL)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

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

# Type:  run spinner.orx &<RET>  kill 0<RET>  exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:s --event key:p --event key:i --event key:n --event key:n --event key:e --event key:r \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x20 --event key:0x26 \
    --event key:0x10D \
    --event key:k --event key:i --event key:l --event key:l \
    --event key:0x20 --event key:0x30 \
    --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 14.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

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

echo "--- shell stderr ---"
cat "$TMP/cpu0.err"

# Extract rendered console.
sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q "\[bg task 0\]"       "$TMP/rendered.txt" \
    || fail "[bg task 0] missing — backgrounded spawn failed"
grep -q "\[task 0 done 137\]" "$TMP/rendered.txt" \
    || fail "auto-reaper didn't print [task 0 done 137] — kill didn't take"

echo "PASS"

#!/bin/sh
# test_shell_exitcode.sh — verify the guest's TaskExit code makes
# it back to the shell's `[exited N]` line.
#
# Compiles a tiny guest that returns 42, runs it from the shell,
# asserts the rendered output contains "[exited 42]". Pre-fix the
# code-propagation chain went: guest TaskExit(42) → simorisc
# captures, then reset_cpu wipes it → loader re-announces with no
# exit info → linkbootd hardcoded 0 in notify_done. Post-fix the
# chain is: simorisc primes R6 with the previous exit code on
# reset → chunkboot.s reads R6 and includes it in the announce →
# linkbootd parses int_payload[2] and uses it in notify_done.

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

# Guest: returns 42 from main; crt0 plumbs that into TaskExit.
cat > "$TMP/exit42.c" <<'EOF'
int
main(void)
{
    return 42;
}
EOF

build_orx() {
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
build_orx "$TMP/exit42.c" "$TMP/jail/exit42.orx"

# Shell + loader.
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (EXIT42)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

python3 examples/linkboot/gen_chunkboot.py >/dev/null
python3 tools/asm/asmorisc examples/linkboot/chunkboot.s -o "$TMP/chunkboot.orx"

SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" -v \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/devices/linkbootd \
    --socket "$SOCK" --pid 18 \
    --shell-pids 0 --loader-pids 32 \
    --root "$TMP/jail" -v \
    > "$TMP/lb.out" 2>&1 &
LB=$!
for _ in $(seq 50); do
    grep -q "linkbootd READY" "$TMP/lb.out" 2>/dev/null && break
    sleep 0.05
done

# Type "run exit42.orx\nexit\n".
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x34 --event key:0x32 --event key:0x2E \
    --event key:o --event key:r --event key:x --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t --event key:0x10D \
    --linger 12.0 --delay 0.25 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

SPARE_SVC="--service 18=1@9 --service 0=0@0 --service 16=1@9 --service 16=2@9 \
           --service 0=0@0 --service 0=0@0 --service 17=1@9"

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" --service "18=1@9" \
    --service "0=0@0" --service "0=0@0" --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

python3 tools/sim/simorisc --connect "$SOCK" --pid 32 \
    $SPARE_SVC --reset-on-exit "$TMP/chunkboot.orx" \
    > "$TMP/cpu32.out" 2> "$TMP/cpu32.err" &
SPARE=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
kill -KILL $CPU0 $SPARE $LB $HF $BAR 2>/dev/null || true
wait $CPU0 $SPARE $LB $HF $BAR 2>/dev/null || true

sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"
echo "--- linkbootd log tail ---"
tail -10 "$TMP/lb.out"

fail() { echo "FAIL: $1" >&2; exit 1; }

# The literal "[exited 42]" must show up. (We accept either with or
# without the trailing newline since the closing "]\n" can race the
# next prompt SEND.)
grep -q "\[exited 42"   "$TMP/rendered.txt" || fail "expected '[exited 42' in render"
! grep -q "\[exited 0"  "$TMP/rendered.txt" || fail "saw stale [exited 0] — exit-code propagation regressed"

echo "PASS"

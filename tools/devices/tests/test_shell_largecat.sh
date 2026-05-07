#!/bin/sh
# test_shell_largecat.sh — regression test for the backpressure
# fix in oriscterm + the chunked term_print_n in the shell.
#
# Cat a ~6KB file (well over oriscterm's socket buffer if you went
# byte-by-byte). Verify all bytes arrive and the system doesn't
# fall over.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/jail"
# 6 KiB file with a recognizable pattern: 6 lines per "stanza",
# 100 stanzas, each starting with a numbered marker.
python3 -c "
import sys
for i in range(100):
    sys.stdout.write(f'--- stanza {i:03d} ---\n')
    sys.stdout.write('the quick brown fox jumps over the lazy dog\n')
    sys.stdout.write('pack my box with five dozen liquor jugs\n')
" > "$TMP/jail/big.txt"
wc -c "$TMP/jail/big.txt"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (LARGECAT)"' \
    ouroboros/shell.c > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
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

# Type "cat big.txt\n" then "exit\n".
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:c --event key:a --event key:t --event key:0x20 \
    --event key:b --event key:i --event key:g --event key:0x2E \
    --event key:t --event key:x --event key:t --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 8.0 --delay 0.05 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
# After fake_terminal exits, the shell may still be alive — under
# load some 'exit' keystrokes get dropped (the shared self-svc queue
# in libc interleaves keys with hostfsd responses; see term.c). Just
# kill it so the test can move on to its assertion.
kill -KILL $CPU 2>/dev/null || true
wait $CPU 2>/dev/null || true
kill -KILL $HF $BAR 2>/dev/null || true
wait $HF $BAR 2>/dev/null || true

# Extract the rendered text and count occurrences of stanza markers.
sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"
COUNT=$(grep -c '^--- stanza ' "$TMP/rendered.txt" || true)
echo "rendered $COUNT stanzas (file has 100)"

# Now that cmd_cat uses term_print_n_sync (which blocks until the
# receiver acks the buffer pull), the SEND/READ race is closed and
# we should reliably see *every* stanza. The threshold accounts for
# only the documented TaskExit/render race on the very last
# stanza — which doesn't apply to cat anyway since the shell
# doesn't exit during it.
[ "$COUNT" -ge 100 ] \
    || { echo "FAIL: only $COUNT stanzas rendered (expected 100)" >&2;
         echo "--- last 20 rendered lines ---" >&2;
         tail -20 "$TMP/rendered.txt" >&2; exit 1; }

# Check no broken-pipe errors in any process's stderr.
if grep -q "Broken pipe\|crossbar closed" "$TMP/term.out"; then
    echo "FAIL: broken-pipe / crossbar-closed in fake_terminal log" >&2
    grep -E "Broken pipe|crossbar closed" "$TMP/term.out" >&2
    exit 1
fi

echo "PASS"

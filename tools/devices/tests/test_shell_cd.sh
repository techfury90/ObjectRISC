#!/bin/sh
# test_shell_cd.sh — exercise the shell's cd / pwd / echo / cycles
# built-ins. Hostfsd is jailed at a fixture dir with a "sub/"
# subdirectory containing a small file.
#
# Typed input:
#     pwd            → expects "/"
#     cd sub         → cwd becomes "/sub"
#     pwd            → expects "/sub"
#     ls             → expects "inner.txt"
#     cat inner.txt  → expects "innerdata"
#     cd ..          → cwd becomes "/"
#     pwd            → expects "/"
#     echo hi there  → expects "hi there"
#     cycles         → expects a positive integer line
#     exit
#
# Asserts each expected line / value appears in the rendered console.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/jail/sub"
printf 'innerdata' > "$TMP/jail/sub/inner.txt"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (CD-TEST)"' \
    examples/cc/shell.c > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    tools/cc/lib/liborisc.ora

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

# Type out the test sequence. RET = 0x10D, space = 0x20, '.' = 0x2E.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:p --event key:w --event key:d --event key:0x10D \
    --event key:c --event key:d --event key:0x20 --event key:s --event key:u --event key:b --event key:0x10D \
    --event key:p --event key:w --event key:d --event key:0x10D \
    --event key:l --event key:s --event key:0x10D \
    --event key:c --event key:a --event key:t --event key:0x20 \
    --event key:i --event key:n --event key:n --event key:e --event key:r \
    --event key:0x2E --event key:t --event key:x --event key:t --event key:0x10D \
    --event key:c --event key:d --event key:0x20 --event key:0x2E --event key:0x2E --event key:0x10D \
    --event key:p --event key:w --event key:d --event key:0x10D \
    --event key:e --event key:c --event key:h --event key:o --event key:0x20 \
    --event key:h --event key:i --event key:0x20 --event key:t --event key:h --event key:e --event key:r --event key:e --event key:0x10D \
    --event key:c --event key:y --event key:c --event key:l --event key:e --event key:s --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t --event key:0x10D \
    --linger 6.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" --service "0=0@0" \
    --service "0=0@0" --service "0=0@0" --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
kill -KILL $CPU $HF $BAR 2>/dev/null || true
wait $CPU $HF $BAR 2>/dev/null || true

sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"

# Assertions. We don't check exact line ordering — just that each
# expected fragment appears somewhere in the rendered stream.
fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q "innerdata"           "$TMP/rendered.txt" || fail "cat inner.txt"
grep -q "inner.txt"           "$TMP/rendered.txt" || fail "ls didn't list inner.txt"
grep -q "^hi there"           "$TMP/rendered.txt" || fail "echo output"
grep -q "/sub>"               "$TMP/rendered.txt" || fail "prompt didn't update after cd sub"
# pwd output before any cd: "/" on its own line.
grep -q "^/$"                 "$TMP/rendered.txt" || fail "pwd at root"
grep -q "^/sub$"              "$TMP/rendered.txt" || fail "pwd after cd sub"
# cycles prints a non-negative integer line.
grep -qE "^[0-9]+$"           "$TMP/rendered.txt" || fail "cycles output"

echo "PASS"

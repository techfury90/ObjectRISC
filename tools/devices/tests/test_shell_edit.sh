#!/bin/sh
# test_shell_edit.sh — backgrounded standalone editor + focus switch.
#
# Phase 40: the editor is a standalone /programs/edit.orx that
# opens a fixed scratchpad (/scratch.txt) and subscribes to the
# keyboard on its own. With the shell already subscribed and the
# editor too, oriscterm has two kbd subscribers; an F1 hotkey
# (mirrored as `--event focus` in fake_terminal) cycles which one
# receives the next keystroke.
#
# Sequence:
#     run /programs/edit.orx &<RET>     ; spawn editor in bg
#     [wait until editor's term_init has subscribed]
#     focus                             ; cycle keyboard to editor
#     ' ' '!' (insert)                  ; goes to the EDITOR, not the shell
#     ^S                                ; editor saves /scratch.txt
#     ^X                                ; editor quits
#     focus                             ; cycle keyboard back to shell
#     exit<RET>                         ; clean shell exit
#
# Asserts /scratch.txt on disk contains the inserted '!'.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

mkdir -p "$TMP/jail/programs"

# Pre-create the scratchpad with a known one-line file.
echo "Hi there" > "$TMP/jail/scratch.txt"

# --- editor program: standalone .orx -----------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/programs/edit.c > "$TMP/edit.i"
"$CCOM" < "$TMP/edit.i" > "$TMP/edit.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/edit.s"                    -o "$TMP/edit.oro"
python3 tools/ld/orld -o "$TMP/jail/programs/edit.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/edit.oro" \
    tools/cc/lib/liborisc.ora

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (FOCUS)"' \
    examples/cc/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    tools/cc/lib/liborisc.ora

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

# Type:  run /programs/edit.orx &<RET>
#        wait-kbd:2 (until editor subscribes)
#        focus (cycle to editor)
#        DOWN-arrow then RIGHT * 8 to land at end of "Hi there", then '!'
#        ^S, ^X
#        focus (cycle back to shell)
#        exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
    --event key:0x2f --event key:e --event key:d --event key:i --event key:t \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x20 --event key:0x26 \
    --event key:0x10D \
    --event wait-kbd:2 \
    --event focus \
    --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x21 \
    --event key:0x13 \
    --event key:0x18 \
    --event focus \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 14.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# Service order: O5=console O6=keyboard O7=grid O8/O9=pad O10=hostfsd.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "0=0@0" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/shell.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
set +m
kill -KILL $CPU0 2>/dev/null || true
wait $CPU0 2>/dev/null || true
for p in $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- shell stderr ---"
cat "$TMP/cpu0.err"

echo "--- saved scratch.txt ---"
cat "$TMP/jail/scratch.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

# The editor opened /scratch.txt (one-line "Hi there"), the user
# scrolled to end of line + inserted '!' + saved. The on-disk
# file should reflect that.
grep -q "^Hi there!" "$TMP/jail/scratch.txt" \
    || fail "scratch.txt doesn't end with the inserted '!'"

# fake_terminal should have logged the focus cycle.
grep -q "kbd focus → 2/2" "$TMP/term.out" \
    || fail "first focus cycle didn't reach 2/2"
grep -q "kbd focus → 1/2" "$TMP/term.out" \
    || fail "second focus cycle didn't return to 1/2"

# The editor should have registered as a 2nd subscriber.
grep -q "now 2 sub(s)" "$TMP/term.out" \
    || fail "editor didn't subscribe to keyboard (multi-sub)"

echo "PASS"

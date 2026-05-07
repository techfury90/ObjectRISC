#!/bin/sh
# test_shell_edit.sh — backgrounded standalone editor + focus switch.
#
# Phase 40: the editor is a standalone /programs/edit.orx that
# subscribes to the keyboard on its own. With the shell already
# subscribed and the editor too, oriscterm has two kbd subscribers;
# an F1 hotkey (mirrored as `--event focus` in fake_terminal)
# cycles which one receives the next keystroke.
#
# Phase 41c-followup also covers two related fixes:
#   - cwd-relative path resolution: we `cd /sub` in the shell first,
#     then `run /programs/edit.orx scratch.txt` (relative path); the
#     editor uses program_cwd() to find the file under /sub.
#   - auto-unsubscribe on exit: edit calls term_shutdown() before
#     TaskExit so oriscterm drops its kbd subscription, and the
#     focus list shrinks back to 1 — no manual F1 cycle needed
#     after the editor quits.
#
# Sequence:
#     cd /sub<RET>                       ; switch shell cwd
#     run /programs/edit.orx scratch.txt &<RET>   ; relative path
#     [wait until editor's term_init has subscribed]
#     focus                              ; cycle keyboard to editor
#     ' ' '!' (insert)                   ; goes to EDITOR, not shell
#     ^S                                 ; editor saves /sub/scratch.txt
#     ^X                                 ; editor quits + unsubscribes
#     exit<RET>                          ; shell still has focus,
#                                        ; this lands without a 2nd F1
#
# Asserts /sub/scratch.txt on disk contains the inserted '!'.

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
mkdir -p "$TMP/jail/sub"

# Pre-create the scratchpad with a known one-line file. Place it
# in /sub so the test exercises cwd-relative path resolution
# (relative `scratch.txt` would otherwise miss against /scratch.txt
# at the jail root).
echo "Hi there" > "$TMP/jail/sub/scratch.txt"

# --- editor program: standalone .orx -----------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib ouroboros/programs/edit.c > "$TMP/edit.i"
"$CCOM" < "$TMP/edit.i" > "$TMP/edit.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/edit.s"                    -o "$TMP/edit.oro"
python3 tools/ld/orld -o "$TMP/jail/programs/edit.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/edit.oro" \
    build/liborisc.ora

# --- shell ------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (FOCUS)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
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

# Type:  cd /sub<RET>
#        run /programs/edit.orx scratch.txt &<RET>
#        wait-kbd:2 (until editor subscribes)
#        focus (cycle to editor)
#        DOWN-arrow then RIGHT * 8 to land at end of "Hi there", then '!'
#        ^S, ^X (editor saves + unsubscribes + exits)
#        exit<RET>  (shell — no 2nd focus needed; auto-unsubscribe
#                  put us back in 1-subscriber-list state)
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:c --event key:d --event key:0x20 \
    --event key:0x2f --event key:s --event key:u --event key:b \
    --event key:0x10D \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
    --event key:0x2f --event key:e --event key:d --event key:i --event key:t \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x20 \
    --event key:s --event key:c --event key:r --event key:a --event key:t --event key:c --event key:h \
    --event key:0x2e --event key:t --event key:x --event key:t \
    --event key:0x20 --event key:0x26 \
    --event key:0x10D \
    --event wait-kbd:2 \
    --event focus \
    --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x183 --event key:0x183 --event key:0x183 --event key:0x183 \
    --event key:0x21 \
    --event key:0x13 \
    --event key:0x18 \
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

echo "--- saved sub/scratch.txt ---"
cat "$TMP/jail/sub/scratch.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

# Editor resolved `scratch.txt` against shell's cwd `/sub` →
# /sub/scratch.txt. After ^S the on-disk file should reflect the
# inserted '!'. (And /scratch.txt at jail root should NOT exist —
# proves the cwd-relative path resolution actually steered the
# write to the right place.)
grep -q "^Hi there!" "$TMP/jail/sub/scratch.txt" \
    || fail "/sub/scratch.txt doesn't end with the inserted '!'"
[ -f "$TMP/jail/scratch.txt" ] && \
    fail "editor wrote to /scratch.txt (should be /sub/scratch.txt)"

# fake_terminal should have logged the (single) focus cycle.
grep -q "kbd focus → 2/2" "$TMP/term.out" \
    || fail "focus cycle didn't reach 2/2 (editor not focused)"

# Editor should have subscribed (now 2 subs) AND unsubscribed on
# exit (back to 1 sub). Both must appear in the log.
grep -q "now 2 sub(s)" "$TMP/term.out" \
    || fail "editor didn't subscribe to keyboard"
grep -q "kbd unsubscribe.*now 1 sub(s)" "$TMP/term.out" \
    || fail "editor didn't unsubscribe on ^X (focus would stay stuck)"

# After unsubscribe, focus should be on the shell — verify by
# checking that the post-quit `exit\n` actually reached the shell
# (without a 2nd manual F1).
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out" \
    | grep -q "exit" \
    || fail "post-quit 'exit' didn't echo to shell — focus stuck"

echo "PASS"

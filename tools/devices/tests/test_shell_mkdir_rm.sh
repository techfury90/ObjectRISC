#!/bin/sh
# test_shell_mkdir_rm.sh — Phase 50: filesystem mutation builtins
# (mkdir / rm / touch) backed by hostfsd's new OP_MKDIR / OP_UNLINK
# and the existing OP_OPEN with O_CREAT.
#
# Drive the shell through a sequence that exercises both creation
# and removal, and verify the results on the host filesystem AFTER
# the simulator exits — that's the most direct check that the wire
# ops actually wrote through to disk.
#
# Keystroke script:
#   ls<RET>                  baseline (note.txt fixture present)
#   mkdir newdir<RET>        create a directory
#   touch newdir/empty<RET>  create an empty file inside it
#   touch newfile<RET>       create another empty file
#   rm note.txt<RET>         remove the fixture file
#   ls<RET>                  show the new state (newdir/, newfile, empty)
#   rm newdir<RET>           refuse — newdir is a directory
#   exit<RET>
#
# Asserts:
#   - On the host filesystem after the run:
#       - jail/newdir/  exists (mkdir worked)
#       - jail/newfile  exists (touch worked)
#       - jail/note.txt is gone (rm worked)
#   - Rendered terminal shows "is a directory" (rm refused on newdir)
#   - Rendered terminal shows "newdir/" in the ls listing (the new dir
#     made it past the ls path)

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/jail"
printf 'a\nb\nc\n' > "$TMP/jail/note.txt"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (TEST)"' \
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
    --socket "$SOCK" --pid 17 --root "$TMP/jail" -v \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# Keystrokes:
#   ls<RET>
#   mkdir newdir<RET>
#   touch newdir/empty<RET>
#   touch newfile<RET>
#   rm note.txt<RET>
#   ls<RET>
#   rm newdir<RET>
#   exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:l --event key:s --event key:0x10D \
    --event key:m --event key:k --event key:d --event key:i --event key:r \
    --event key:0x20 --event key:n --event key:e --event key:w \
    --event key:d --event key:i --event key:r --event key:0x10D \
    --event key:t --event key:o --event key:u --event key:c --event key:h \
    --event key:0x20 --event key:n --event key:e --event key:w \
    --event key:d --event key:i --event key:r --event key:0x2f \
    --event key:e --event key:m --event key:p --event key:t --event key:y \
    --event key:0x10D \
    --event key:t --event key:o --event key:u --event key:c --event key:h \
    --event key:0x20 --event key:n --event key:e --event key:w \
    --event key:f --event key:i --event key:l --event key:e --event key:0x10D \
    --event key:r --event key:m --event key:0x20 --event key:n --event key:o \
    --event key:t --event key:e --event key:0x2e --event key:t --event key:x \
    --event key:t --event key:0x10D \
    --event key:l --event key:s --event key:0x10D \
    --event key:r --event key:m --event key:0x20 \
    --event key:n --event key:e --event key:w --event key:d \
    --event key:i --event key:r --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t --event key:0x10D \
    --linger 3.0 --delay 0.10 \
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
sleep 0.3
wait $CPU 2>/dev/null || true
kill -TERM $HF 2>/dev/null || true
wait $HF 2>/dev/null || true
kill -TERM $BAR 2>/dev/null || true
wait $BAR 2>/dev/null || true

echo "--- terminal render ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out"
echo "--- jail tree after run ---"
( cd "$TMP/jail" && find . -mindepth 1 | sort )

# 1) The host-side filesystem reflects the operations.
[ -d "$TMP/jail/newdir" ] \
    || { echo "FAIL: mkdir didn't create newdir/" >&2; exit 1; }
[ -f "$TMP/jail/newfile" ] \
    || { echo "FAIL: touch didn't create newfile" >&2; exit 1; }
[ -f "$TMP/jail/newdir/empty" ] \
    || { echo "FAIL: touch newdir/empty didn't create the file " \
              "(parent mkdir worked, but path-into-newdir didn't)" >&2; exit 1; }
[ ! -e "$TMP/jail/note.txt" ] \
    || { echo "FAIL: rm didn't remove note.txt" >&2; exit 1; }

# 2) The shell printed the "is a directory" refusal for `rm newdir`.
RENDER=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out")
echo "$RENDER" | grep -q "is a directory" \
    || { echo "FAIL: rm didn't refuse on directory target" >&2; exit 1; }

# 3) The post-rm `ls` listing in the render mentions newdir/ — proves
#    the new dir made it through and ls saw it (the listing's other
#    contents are timing-sensitive, so we only assert one entry).
echo "$RENDER" | grep -q "newdir/" \
    || { echo "FAIL: 'ls' after mkdir didn't list newdir/" >&2; exit 1; }

echo "PASS"

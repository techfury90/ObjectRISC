#!/bin/sh
# test_shell_bg.sh — backgrounded `run cmd &` + `wait <task>` flow.
#
# Builds:
#   - shell.orx
#   - hello_term.orx (uses term_print_only_init + term_print)
#
# Types:
#     run hello_term.orx &<RET>     ; spawns guest; shell prints
#                                   ; "[bg task N]" and yields once
#     wait 0<RET>                   ; harvests exit code
#     exit<RET>
#
# Asserts the rendered terminal contains:
#   - "[bg task 0]"                  (cmd_run with & path)
#   - "hello from inside the Tk window"  (the guest itself)
#   - task 0 reaped exactly once, exit 0, via EITHER verb:
#     "[task 0 done 0]" (auto-reaper) or "[task 0 exited 0]" (explicit
#     wait). Which reaper wins is a cooperative-scheduling race — both
#     mean the bg task ran and was harvested cleanly. orx_unload frees
#     the task, so only one of the two fires (exactly once). Pinning a
#     specific verb made this assert an unpromised scheduling detail and
#     flip on unrelated libc cycle-count changes.

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

# --- guest: term_print_only_init + term_print -------------------------
cat > "$TMP/hello_term.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    term_print_only_init();
    term_print("hello from inside the Tk window\n");
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

build_guest "$TMP/hello_term.c" "$TMP/jail/hello_term.orx"

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

# Type:  run hello_term.orx &<RET>  wait 0<RET>  exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x5f --event key:t --event key:e --event key:r --event key:m \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x20 --event key:0x26 \
    --event key:0x10D \
    --event key:w --event key:a --event key:i --event key:t \
    --event key:0x20 --event key:0x30 \
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

# Spawn announcement: cmd_run's `&` path printed "[bg task N]". We match
# only the "[bg task " PREFIX, not the contiguous "[bg task 0]" — the bg
# guest runs concurrently, and its output can interleave between the
# prefix and the "N]" when the preempt timer fires mid-announcement (a
# scheduling artifact, not a failure; it shifts on any libc cycle-count
# change). The task NUMBER and clean exit are pinned by the reap invariant
# below. Still fails if no bg task was announced at all (a real spawn
# failure). [bg task NN comes through as one term_print, so the prefix
# itself is never split.]
grep -q "\[bg task "                   "$TMP/rendered.txt" \
    || fail "no '[bg task' announcement — cmd_run's & path didn't spawn a bg task"
grep -q "hello from inside the Tk"     "$TMP/rendered.txt" \
    || fail "guest term_print didn't reach the Tk window"

# Reap invariant: task 0 is reaped exactly once, with exit code 0, by
# EITHER path — the auto-reaper ("[task 0 done 0]") or an explicit
# `wait 0` ("[task 0 exited 0]"). Which one wins is a scheduling race;
# both prove the bg task ran and was harvested cleanly. orx_unload frees
# the task, so only one of the two can fire — hence exactly once. (We do
# NOT accept "the digit 0 appears somewhere": no-reap, a nonzero code,
# and a double reap must all still fail.)
reap_all=$(grep -oE "\[task 0 (done|exited) -?[0-9]+\]" "$TMP/rendered.txt" | wc -l | tr -d ' ')
reap_zero=$(grep -oE "\[task 0 (done|exited) 0\]" "$TMP/rendered.txt" | wc -l | tr -d ' ')
[ "$reap_all" -eq 1 ] \
    || fail "task 0 should be reaped exactly once, saw $reap_all reap line(s)"
[ "$reap_zero" -eq 1 ] \
    || fail "task 0 not reaped with exit 0 (reap line carried a nonzero code)"

echo "PASS"

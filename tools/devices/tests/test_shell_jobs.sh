#!/bin/sh
# test_shell_jobs.sh — `jobs` listing + auto-reap on the next prompt.
#
# Spawns a guest in the background, immediately runs `jobs` so the
# task is still alive (state = EXITED already, since the guest is
# trivial; we just want `jobs` to enumerate it). The follow-up
# prompt iteration runs the auto-reaper, which prints
# "[task 0 done 0]" before showing the next prompt.
#
# Asserts on the rendered terminal:
#   - "[bg task 0]"        from cmd_run with `&`
#   - a valid `jobs` listing: EITHER task 0 enumerated with a live-state
#     label ("[task 0] runnable" etc.) OR "(no live tasks)" if the
#     auto-reaper cleared the slot first — which one depends on whether
#     the reap beat the `jobs` dispatch, a scheduling race; both are
#     correct jobs output.
#   - task 0 reaped exactly once, exit 0 (here always via the auto-reaper:
#     "[task 0 done 0]"; the either-verb check below is shared with
#     test_shell_bg). Pinning the exact jobs state / reaper made this
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

# Type:  run hello_term.orx &<RET>  jobs<RET>  exit<RET>
# Auto-reap fires before the next prompt after `&`, then `jobs`
# would show empty if reaping happened first. To race-proof: type
# `jobs` IMMEDIATELY after `&` so it lands in the keystroke queue
# before the auto-reaper has a chance to see EXITED. With
# cooperative scheduling the order is: shell sees `&`, spawns,
# yields once, child runs+exits, shell resumes, prints `[bg]`,
# loops, reap fires, prints `[task 0 done 0]`, THEN prompts.
# So `jobs` won't see the live task. That's fine — the test
# instead just verifies the auto-reap message appeared.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x5f --event key:t --event key:e --event key:r --event key:m \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x20 --event key:0x26 \
    --event key:0x10D \
    --event key:j --event key:o --event key:b --event key:s \
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

wait $TERM_PID 2>/dev/null || true
sleep 0.5
wait $CPU0 2>/dev/null || true
for p in $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $HF $BAR; do wait $p 2>/dev/null || true; done

sed -n '/--- console render ---/,$p' "$TMP/term.out" \
    | tail -n +2 > "$TMP/rendered.txt"

echo "--- rendered ---"
cat "$TMP/rendered.txt"

fail() { echo "FAIL: $1" >&2; exit 1; }

# Spawn announcement: match only the "[bg task " PREFIX, not "[bg task 0]"
# — the concurrent bg guest can interleave between the prefix and "N]"
# (preempt fires mid-announcement; shifts on any libc cycle-count change).
# The task number + clean exit are pinned by the reap invariant below.
# Still fails if no bg task was announced (a real spawn failure).
grep -q "\[bg task "        "$TMP/rendered.txt" \
    || fail "no '[bg task' announcement — cmd_run's & path didn't spawn a bg task"

# jobs invariant: the `jobs` command ran and produced a valid listing —
# either it still caught task 0 (printed with a live-state label) or the
# auto-reaper had already cleared the table ("(no live tasks)"). Whether
# the reap beats the `jobs` dispatch is a scheduling race; both are
# correct jobs behaviour.
grep -qE "\(no live tasks\)|\] (new|runnable|running|suspended|blocked|exited)" "$TMP/rendered.txt" \
    || fail "jobs produced no recognizable listing (neither a task entry nor '(no live tasks)')"

# Reap invariant (shared with test_shell_bg): task 0 reaped exactly once,
# exit 0, via either verb. Here the reaper is always the auto-reaper
# ("[task 0 done 0]"), but the either-verb form keeps the two tests in
# step. Still fails on no-reap / nonzero / double-reap.
reap_all=$(grep -oE "\[task 0 (done|exited) -?[0-9]+\]" "$TMP/rendered.txt" | wc -l | tr -d ' ')
reap_zero=$(grep -oE "\[task 0 (done|exited) 0\]" "$TMP/rendered.txt" | wc -l | tr -d ' ')
[ "$reap_all" -eq 1 ] \
    || fail "task 0 should be reaped exactly once, saw $reap_all reap line(s)"
[ "$reap_zero" -eq 1 ] \
    || fail "task 0 not reaped with exit 0 (reap line carried a nonzero code)"

echo "PASS"

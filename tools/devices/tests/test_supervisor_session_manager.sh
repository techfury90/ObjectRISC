#!/bin/sh
# test_supervisor_session_manager.sh — regression test for the spawn
# vs. shutdown-relay race that the leader's supervisor used to lose
# whenever its boot path included a slow-to-load `.orx`.
#
# Repro shape:
#   - 2 CPUs, both terminal-equipped.
#   - sysinit.orx is replaced with a "session_manager" whose only
#     non-trivial cost is that it transitively links the .orx loader
#     (via an unreachable sup_spawn). This bloats its text section by
#     ~24 KiB, so the leader's hostfs-driven .orx load runs long
#     enough that the worker's shell finishes its session and relays
#     op=2 (sup_shutdown) to the leader BEFORE the leader's own shell
#     has a chance to start.
#   - shell.orx prints "S0\n" to firmware stdout as the very first
#     thing main() does — before task_init. Its appearance in cpu0.out
#     is the proof that the second spawn's main() actually ran.
#
# Without the fix:
#   handle_spawn_request task_resumes the new shell, returns, the
#   supervisor's next poll picks up the queued op=2 immediately, and
#   the cascade-kill takes the shell from NEW straight to EXITED
#   without ever scheduling it. cpu0.out never sees "S0".
#
# With the fix (handle_spawn_request task_yields after task_resume):
#   the just-resumed shell runs through crt0 + main's first console_write
#   before blocking on the keyboard. cpu0.out contains "S0" even when
#   the cascade-kill races in immediately afterwards.
#
# The assertion is invariant-based, not cycle-pinned (this test used to
# flake ~1-in-5 under full-suite host load and broke deterministically
# under libc cycle-count changes, because it grepped S0 unconditionally
# even when the leader legitimately shut down before resuming any shell).
# A test-injected SPAWN_RESUMED marker (see the supervisor build below)
# distinguishes "resumed a shell that never ran" (the bug → FAIL) from
# "never resumed a shell" (shutdown won the race → not applicable). See
# the assertion block at the bottom for the full rationale.

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

# --- session_manager.c (replaces sysinit.orx) ------------------------
# Minimal: task_init, exit. The unreachable sup_spawn forces orld to
# pull in the entire .orx loader subtree, which is what makes this
# binary big enough to expose the race. Without the unreachable call,
# sup_spawn isn't referenced and the linker doesn't pull it in.
cat > "$TMP/session_manager.c" <<'EOF'
#include "liborisc.h"

extern int _orisc_init_r4;

int
main(void)
{
    task_init();
    print_str("session_manager: online\n");
    /* Unreachable in source; pulls sup_spawn (and its transitive
     * deps — orx_spawn et al) into the linker output. */
    if (_orisc_init_r4 == (int)0xDEADBEEF) {
        sup_spawn("/dev/null", "", "/");
    }
    return 0;
}
EOF

# --- shell.c → adds S0 marker to track main()-entry on each spawn ---
cp ouroboros/shell.c "$TMP/shell.c"
# Insert console_write("S0\n", 3) at the top of main(), before
# task_init. We pick the unique line "char line[LINE_MAX];" as the
# anchor — it's the first declaration inside main and only appears
# there in the file.
awk '
  /^[[:space:]]+char line\[LINE_MAX\];/ && !done {
      print "extern int console_write(const char *buf, int count);";
      print $0;
      print "\tconsole_write(\"S0\\n\", 3);";
      done = 1
      next
  }
  { print }
' "$TMP/shell.c" > "$TMP/shell_with_marker.c"

# --- common build helpers --------------------------------------------
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"

build_orx() {
    src="$1"; out="$2"
    "$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/__pp.i"
    "$CCOM" < "$TMP/__pp.i" > "$TMP/__pp.s"
    python3 tools/asm/asmorisc -r "$TMP/__pp.s" -o "$TMP/__main.oro"
    python3 tools/ld/orld -o "$out" \
        "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/__main.oro" \
        build/liborisc.ora
}

build_shell() {
    "$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib \
        -DBUILD_BANNER='"Object RISC Shell (TEST)"' \
        "$1" > "$TMP/__pp.i"
    "$CCOM" < "$TMP/__pp.i" > "$TMP/__pp.s"
    python3 tools/asm/asmorisc -r "$TMP/__pp.s" -o "$TMP/__main.oro"
    python3 tools/ld/orld -o "$2" \
        "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/__main.oro" \
        build/liborisc.ora
}

# Wire session_manager as /programs/sysinit.orx (the supervisor's
# leader-only first user task) and the marker-emitting shell as
# /programs/shell.orx.
build_orx "$TMP/session_manager.c"          "$TMP/jail/programs/sysinit.orx"
build_orx "ouroboros/programs/login.c"      "$TMP/jail/programs/login.orx"
build_shell "$TMP/shell_with_marker.c"      "$TMP/jail/programs/shell.orx"

# --- supervisor ------------------------------------------------------
# Inject a SPAWN_RESUMED marker right after handle_spawn_request's
# task_resume (the op=1 / sup_spawn path — which in this scenario fires
# ONLY for the shell, since the supervisor resumes sysinit/login directly
# in main()). The marker prints whenever the leader actually resumed a
# shell, BEFORE the task_yield that lets it run — so it appears in both
# the fixed build (resume → yield → shell runs → S0) and the buggy build
# (resume → no yield → cascade-kill before the shell runs, no S0). It is
# ABSENT only when no shell was ever resumed (the worker's relayed op=2
# shutdown won the race to the leader's mailbox before login's spawn
# request). That distinction is what lets the assertion below tell the
# genuine bug from the harmless "shutdown raced in first" case — they are
# otherwise byte-identical on stdout. SUP_PRINT restores O3 from O15
# (clobbered by orx_spawn) and is the supervisor's own print idiom, so
# this is safe mid-handler and does not change the resume→yield ordering.
awk '
  /if \(t >= 0\) \(void\)task_resume\(t\);/ && !done {
      print $0
      print "\tif (t >= 0) SUP_PRINT(\"SPAWN_RESUMED\\n\");"
      done = 1
      next
  }
  { print }
' ouroboros/supervisor.c > "$TMP/supervisor_marked.c"

"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    "$TMP/supervisor_marked.c" > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + oriscdir + hostfsd + 2 fake terminals --------
SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 \
    --config tools/devices/oriscdir.default.conf \
    > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do
    grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/devices/hostfsd --directory-pid 18 --instance 0 \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# Both terminals: dismiss login's welcome banner with <RET>, then
# type `exit<RET>` so each shell calls sup_shutdown and the worker
# relays op=2 to the leader. The leader sees its own shell's
# op=2 AND the worker's relayed op=2; that mailbox-already-non-empty
# state is exactly what the bug needs to manifest.
KEYS="--event key:0x10D \
      --event key:e --event key:x --event key:i --event key:t \
      --event key:0x10D"

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 \
    $KEYS \
    --linger 8.0 --delay 0.20 \
    > "$TMP/term16.out" 2>&1 &
TERM16=$!

python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 19 \
    --directory-pid 18 --instance 1 \
    $KEYS \
    --linger 8.0 --delay 0.20 \
    > "$TMP/term19.out" 2>&1 &
TERM19=$!

for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term16.out" 2>/dev/null \
        && grep -q "fake_terminal READY" "$TMP/term19.out" 2>/dev/null \
        && break
    sleep 0.05
done

# --- two CPUs, each wired to a different terminal -------------------
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "19=1@9" --service "19=2@9" \
    --service "19=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM16 2>/dev/null || true
wait $TERM19 2>/dev/null || true
sleep 0.5
wait $CPU0 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"

# The guarantee under test: handle_spawn_request must task_yield after
# task_resume, so a freshly-resumed shell runs its first instruction (S0,
# injected at the very top of main) before a queued op=2 shutdown
# cascade-kills it.
#
# We assert the INVARIANT, not the exact cross-CPU cycle race (which
# shifts under host load and any libc cycle-count change — the source of
# this test's historical ~1-in-5 full-suite flake). The SPAWN_RESUMED
# marker tells two otherwise-identical stdout shapes apart:
#
#   - SPAWN_RESUMED present, S0 absent  → the leader resumed a shell but
#     it never ran: the genuine bug (resume not followed by a yield). FAIL.
#   - SPAWN_RESUMED absent               → the leader never resumed a shell
#     (the worker's relayed op=2 reached the mailbox before login's spawn
#     request, so the cascade-kill fired first). No shell was ever at
#     risk; the guarantee is not applicable. NOT a failure — this is the
#     timing-dependent case the old unconditional `grep S0` flipped on.
#
# The worker (CPU 1) has no inbound relayed op=2 during its own shell
# spawn, so it ALWAYS resumes and runs its shell — we assert it strictly
# (SPAWN_RESUMED *and* S0). That keeps the test from going vacuous: every
# run exercises and checks the resume→run guarantee on CPU 1, and the
# negative build (remove the task_yield) is caught on CPU 0 (SPAWN_RESUMED
# present, S0 missing).
fail() { echo "FAIL: $1" >&2; exit 1; }

# CPU 1 (worker): the guarantee always applies here — strict check.
grep -q '^SPAWN_RESUMED$' "$TMP/cpu1.out" \
    || fail "CPU 1 (worker) never resumed a shell — test setup broken (expected a reliable worker shell spawn)"
grep -q '^S0$' "$TMP/cpu1.out" \
    || fail "CPU 1 (worker) resumed a shell but it never reached main() (S0)"

# CPU 0 (leader): conditional on a shell actually being resumed, since the
# spawn-vs-shutdown race may legitimately shut the leader down first.
if grep -q '^SPAWN_RESUMED$' "$TMP/cpu0.out"; then
    grep -q '^S0$' "$TMP/cpu0.out" \
        || fail "CPU 0 (leader) resumed a shell but it never reached main() (S0) — handle_spawn_request didn't let it run before the shutdown cascade"
fi

echo "PASS"

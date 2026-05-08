#!/bin/sh
# test_supervisor_run_at.sh — Phase 45e: cross-CPU spawn via shell
# `run @N command` syntax.
#
# Architecture under test: every CPU boots supervisor.orx (Phase 45c),
# spawn mailbox is at deterministic descriptor idx 5 (Phase 45e), boot
# OPRs include each CPU's peer-supervisor sub-cap at O8, supervisor
# harvests it into PEER_SUP_SLOT. The shell parses `run @N path` and
# packs N into R6 of the op=1 SEND. The local supervisor receives,
# sees target_pid != self.procid, and relays to PEER_SUP_SLOT. The
# peer reads bytes via ObjFetchBytes (remote OBJ_READ_REQ to the
# original requester's bytes object), spawns locally, replies directly
# to the shell's reply mailbox. The returned task ref has home = peer
# CPU; orx_unload's task_wait routes via Phase 45d's remote TaskWait.
#
# Builds:
#   - supervisor.orx (boot leader on every CPU)
#   - shell.orx     (leader-only first task)
#   - hello.orx     (printed identifier includes 'run @N')
#
# Launches: oriscbar + hostfsd + fake_terminal + 2 simorisc CPUs.
# Types:    run @1 /programs/hello.orx<RET>  exit<RET>
#
# Asserts:
#   - cpu0 stdout contains "supervisor: booting (leader)"
#   - cpu1 stdout contains "supervisor: booting (worker)"
#   - cpu1 stdout contains "hello-from-supervised-spawn"
#     (the spawn happened on CPU 1, NOT CPU 0)
#   - cpu0 stdout does NOT contain "hello-from-supervised-spawn"
#     (would indicate the relay didn't happen)
#   - cpu0 stdout contains "supervisor: shell exited; halting"

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

# --- guest --------------------------------------------------------------
cat > "$TMP/hello.c" <<'EOF'
#include "liborisc.h"

int
main(void)
{
    print_str("hello-from-supervised-spawn\n");
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

build_guest "$TMP/hello.c" "$TMP/jail/programs/hello.orx"

# --- shell --------------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DBUILD_BANNER='"Object RISC Shell (TEST)"' \
    ouroboros/shell.c > "$TMP/shell.i"
"$CCOM" < "$TMP/shell.i" > "$TMP/shell.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/shell.s"                   -o "$TMP/shell.oro"
python3 tools/ld/orld -o "$TMP/jail/programs/shell.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/shell.oro" \
    build/liborisc.ora

# --- supervisor ---------------------------------------------------------
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib \
    ouroboros/supervisor.c > "$TMP/sup.i"
"$CCOM" < "$TMP/sup.i" > "$TMP/sup.s"
python3 tools/asm/asmorisc -r "$TMP/sup.s" -o "$TMP/sup.oro"
python3 tools/ld/orld -o "$TMP/supervisor.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/sup.oro" \
    build/liborisc.ora

# --- launch oriscbar + hostfsd + fake_terminal --------------------------
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

# Phase 45f: launch the directory daemon. Both supervisors will
# register themselves at /sys/cpu/<procid>/supervisor and the
# leader looks up its peer via dir_walk on the same path.
python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 -v \
    > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do
    grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break
    sleep 0.05
done

# Type:  ls<RET>                         (Phase 45g: exercises the
#                                          lazy-bootstrap path —
#                                          shell's first vfs_* call
#                                          SENDs op=4 SUP_OP_GET_DIR
#                                          to the supervisor; without
#                                          a handler the shell would
#                                          hang here)
#        cd programs<RET>
#        ls<RET>                         (MOUNT-backed listing — fills
#                                          cmd_ls's stack LIST_BUF with
#                                          a much larger hostfsd dump)
#        cd /<RET>
#        ls<RET>                         (back to DIR — a buggy vfs_list
#                                          would return the wrong byte
#                                          count and bleed leftover
#                                          /programs entries through)
#        run @1 /programs/hello.orx<RET>
#        exit<RET>
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 \
    --event key:l --event key:s --event key:0x10D \
    --event key:c --event key:d --event key:0x20 --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s --event key:0x10D \
    --event key:l --event key:s --event key:0x10D \
    --event key:c --event key:d --event key:0x20 --event key:0x2f --event key:0x10D \
    --event key:l --event key:s --event key:0x10D \
    --event key:r --event key:u --event key:n --event key:0x20 \
    --event key:0x40 --event key:0x31 --event key:0x20 \
    --event key:0x2f --event key:p --event key:r --event key:o --event key:g --event key:r --event key:a --event key:m --event key:s \
    --event key:0x2f --event key:h --event key:e --event key:l --event key:l --event key:o \
    --event key:0x2e --event key:o --event key:r --event key:x \
    --event key:0x10D \
    --event key:e --event key:x --event key:i --event key:t \
    --event key:0x10D \
    --linger 15.0 --delay 0.20 \
    > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do
    grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break
    sleep 0.05
done

# Phase 45f: both CPUs have an identical service map. O8 carries
# the directory daemon's primary mailbox sub-cap ("18=1@9") rather
# than a static peer ref — peer discovery is now via dir_walk on
# /sys/cpu/<N>/supervisor. The supervisor harvests O8 into
# DIR_SLOT at boot and uses dir_register to publish its own
# mailbox under /sys/cpu/<procid>/supervisor; relay_spawn_request
# does dir_walk for the peer's ref.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "16=1@9" --service "16=2@9" \
    --service "16=3@9" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu1.out" 2>"$TMP/cpu1.err" &
CPU1=$!

wait $TERM_PID 2>/dev/null || true
sleep 0.5
wait $CPU0 2>/dev/null || true
kill -KILL $CPU1 2>/dev/null || true
wait $CPU1 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- oriscdir log ---"
cat "$TMP/dir.out"
echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu0 stderr ---"
cat "$TMP/cpu0.err"
echo "--- cpu1 stdout ---"
cat "$TMP/cpu1.out"
echo "--- cpu1 stderr ---"
cat "$TMP/cpu1.err"
echo "--- rendered ---"
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out"

# 1) Boot announcements with the right roles.
grep -q "supervisor: booting (leader)" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 didn't announce as leader" >&2; exit 1; }
grep -q "supervisor: booting (worker)" "$TMP/cpu1.out" \
    || { echo "FAIL: cpu1 didn't announce as worker" >&2; exit 1; }

# 2) The spawn happened on CPU 1 (peer), not CPU 0.
grep -q "hello-from-supervised-spawn" "$TMP/cpu1.out" \
    || { echo "FAIL: hello didn't print on cpu1 (the relay target)" >&2; exit 1; }
grep -q "hello-from-supervised-spawn" "$TMP/cpu0.out" \
    && { echo "FAIL: hello printed on cpu0; relay didn't fire" >&2; exit 1; }
true

# 3) Leader supervisor wound down on shell exit.
grep -q "supervisor: shell exited; halting" "$TMP/cpu0.out" \
    || { echo "FAIL: leader didn't shut down" >&2; exit 1; }

# 4) Phase 45g regression check: the shell's post-spawn term_prints
#    must actually land on the terminal. The shell prints "[exited N]"
#    after a foreground sup_spawn returns, and "[task N done CODE]"
#    when the auto-reaper notices an exit. If sup_spawn clobbers O15
#    (task.c's boot-data save), term.c's _term_restore_or feeds the
#    wrong source ref to oriscterm and these prints fail silently.
#    Without this assertion the bug shipped — fake_terminal drops
#    BOUNDS-failed prints quietly, so earlier 45f/g tests passed even
#    when the shell couldn't print after a spawn under `make boot`.
sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out" \
    | grep -q "exited 0" \
    || { echo "FAIL: shell didn't print [exited 0] after run @1 — sup_spawn likely clobbered a boot OR slot" >&2; exit 1; }

# 5) Phase 45g regression check: lazy bootstrap. The first `ls` from
#    the shell triggers vfs_list → dir_walk → dir_init's
#    SUP_OP_GET_DIR (op=4) to the supervisor. Without an op=4 handler
#    the shell hangs forever waiting for a reply (and the supervisor
#    prints "supervisor: unknown op"). With the handler the shell
#    receives the directory ref, populates DIR_SLOT, and dir_walks
#    the root — which has `programs/` registered as a MOUNT and
#    `sys/` as a DIR. So `ls` should produce a listing containing
#    "programs".
RENDER=$(sed -n '/--- console render ---/,/--- grid render ---/p' "$TMP/term.out")
echo "$RENDER" | grep -q "programs" \
    || { echo "FAIL: ls didn't list 'programs' — lazy bootstrap (op=4) likely broken" >&2; exit 1; }
grep -q "supervisor: unknown op" "$TMP/cpu0.out" \
    && { echo "FAIL: supervisor logged 'unknown op' — op=4 handler missing" >&2; exit 1; }
true

# 6) Phase 45g regression check: ls output is newline-separated, not
#    NUL-collapsed. oriscdir's dir_list returns NUL-separated names;
#    vfs_list translates NULs to newlines for display so each entry
#    lands on its own line. Without that translation the terminal
#    eats NULs and renders "programs/sys/" instead of one-per-line.
#    Match an exact "programs/" line — anchored to a line boundary —
#    so the assertion fails if entries glom together.
echo "$RENDER" | grep -qE '^programs/$' \
    || { echo "FAIL: 'programs/' not on its own line — ls entries collapsed (vfs_list NUL→\\n translation broken)" >&2; exit 1; }

# 7) Phase 45g regression check: vfs_list's DIR branch must return
#    the EXACT byte count of the listing, not walk past the end into
#    leftover stack from a previous MOUNT-backed listing. Repro:
#    `ls` (DIR) → `cd programs` → `ls` (MOUNT, fills LIST_BUF with
#    file names) → `cd /` → `ls` (DIR again, only writes 15 bytes,
#    leftover is everything past). A buggy length calculation prints
#    the leftover too. Verify by counting occurrences of "hello.orx"
#    in the rendering — it should appear EXACTLY ONCE (the
#    `/programs` ls + the `run` line both count as the same string?
#    no: "hello.orx" only appears in the file listing and the run
#    command echo. Count "shell.orx" instead, which is unique to
#    the file listing and would appear a second time if leftover
#    leaked into the second root ls. */
SHELL_ORX_COUNT=$(echo "$RENDER" | grep -c "shell.orx" || true)
if [ "$SHELL_ORX_COUNT" -gt 1 ]; then
    echo "FAIL: 'shell.orx' appears $SHELL_ORX_COUNT times — vfs_list DIR branch is leaking leftover MOUNT-listing bytes" >&2
    exit 1
fi

echo "PASS"

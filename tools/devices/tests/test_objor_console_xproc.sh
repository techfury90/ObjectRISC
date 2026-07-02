#!/bin/sh
# test_objor_console_xproc.sh — object-console Phase-2 cross-process proof.
#
# Builds the supervisor with -DCONSOLE_PROOF, which (only in this test build)
# replaces the login/shell spawn with a boot hook that spawns
# /programs/console_launcher.orx WITH the full spawn environment (O8=directory
# injected, like sysinit), blocks on it, reports its exit code, and halts.
#
# The launcher (examples/cc/objor_console_xproc.c) stands up a result-sink,
# hands its send-cap to a SEPARATE command program (/programs/command.orx,
# examples/cc/objor_console_cmd.c) via boot register O9, orx_spawns it, and
# collects + verifies the typed results the command streams back. This proves
# the boot-cap result-sink ABI across a REAL orx_spawn process boundary (vs
# objor_console.c's single-program task_spawn PoC).
#
# Passes iff cpu0 reports "console-proof: launcher exited 42" (the launcher
# returns 42 only after collecting + verifying all 3 results). No keystrokes:
# the launcher runs automatically; the terminal only provides /sys/term/0
# (so the supervisor is has_terminal and reaches the hook) and renders.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"
if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    echo "SKIP: pcc not built at $PCC_BUILD (run tools/cc/build.sh)" >&2
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
mkdir -p "$TMP/jail/programs"

python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"

build_orx() {
    src="$1"; out="$2"; shift 2
    "$CPP" "$@" -I tools/cc/arch/orisc -I tools/cc/lib "$src" > "$TMP/__pp.i"
    "$CCOM" < "$TMP/__pp.i" > "$TMP/__pp.s"
    python3 tools/asm/asmorisc -r "$TMP/__pp.s" -o "$TMP/__main.oro"
    python3 tools/ld/orld -o "$out" \
        "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/__main.oro" build/liborisc.ora
}

# The proof programs + the CONSOLE_PROOF-gated supervisor.
build_orx examples/cc/objor_console_cmd.c   "$TMP/jail/programs/command.orx"
build_orx examples/cc/objor_console_xproc.c "$TMP/jail/programs/console_launcher.orx"
build_orx ouroboros/supervisor.c            "$TMP/supervisor.orx" -DCONSOLE_PROOF

# --- launch bus + devices ----------------------------------------------
SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 -v \
    --config tools/devices/oriscdir.default.conf > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/devices/hostfsd --directory-pid 18 --instance 0 \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break; sleep 0.05; done

# No keystrokes — the launcher runs automatically. The terminal only supplies
# /sys/term/0 (so the supervisor walks a console => has_terminal => the hook)
# and renders.
python3 tools/devices/tests/fake_terminal.py \
    --socket "$SOCK" --pid 16 --directory-pid 18 --instance 0 \
    --linger 20.0 > "$TMP/term.out" 2>&1 &
TERM_PID=$!
for _ in $(seq 50); do grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break; sleep 0.05; done

# Single CPU, walk-don't-wire (null O5/O6/O7 -> walks /sys/term/0; O8=dir,
# O10=hostfsd wired). See test_supervisor_run_at.sh.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "18=1@9" --service "0=0@0" \
    --service "17=1@9" \
    "$TMP/supervisor.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err" &
CPU0=$!

# The supervisor halts (return 0) once the launcher finishes; wait for that,
# then stop the still-lingering terminal + devices.
wait $CPU0 2>/dev/null || true
kill -KILL $TERM_PID 2>/dev/null || true
for p in $DIR $HF $BAR; do kill -KILL $p 2>/dev/null || true; done
for p in $TERM_PID $DIR $HF $BAR; do wait $p 2>/dev/null || true; done

echo "--- cpu0 stdout ---"; cat "$TMP/cpu0.out"
echo "--- cpu0 stderr ---"; cat "$TMP/cpu0.err"

# 1) Supervisor booted.
grep -q "supervisor: booting" "$TMP/cpu0.out" \
    || { echo "FAIL: cpu0 supervisor didn't boot" >&2; exit 1; }

# 2) The launcher completed the whole cross-process proof (sink handoff via
#    O9, orx_spawn'd command, results collected + verified) and exited 42.
grep -q "console-proof: launcher exited 42" "$TMP/cpu0.out" \
    || { echo "FAIL: launcher didn't exit 42 (see cpu0 output above; the O9 sink handoff, orx_spawn, or result collection failed)" >&2; exit 1; }

echo "PASS"

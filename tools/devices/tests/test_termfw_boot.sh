#!/bin/sh
# test_termfw_boot.sh — M2: co-resident terminal boot.
#
# The terminal firmware (termfw.orx) runs its VRAM self-test + splash, then
# loads and runs the supervisor as a SAME-CPU task (orx_spawn), forwarding the
# directory cap so the supervisor harvests O8 = directory.  The supervisor boots
# CO-RESIDENT on the firmware CPU and spawns sysinit — proving the spawn service
# works on one CPU.  The directory cap is the only wired input; hostfsd and the
# /programs mount derive from it.
#
# Architecture under test:
#   - oriscbar (crossbar), oriscdir (pid 18), hostfsd (pid 17, jailed)
#   - simorisc CPU 0 = termfw.orx  ->  supervisor.orx (task)  ->  sysinit (task)
#
# Standalone splash (no supervisor) is covered by test_termfw_splash.sh.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
make -s all >/dev/null

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
mkdir -p "$TMP/jail/programs"

# Build termfw.orx in M2 mode (NO -DSTOP_AFTER_SPLASH) with a short self-test.
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" -I tools/cc/arch/orisc -I tools/cc/lib \
    -DDELAY_US=2000 ouroboros/termfw.c > "$TMP/t.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/t.i" > "$TMP/t.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/t.s"                       -o "$TMP/t.oro"
python3 tools/ld/orld -o "$TMP/termfw.orx" "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/t.oro" build/liborisc.ora

# /programs (jail): the supervisor boot image + what it spawns at boot.
cp build/supervisor.orx       "$TMP/jail/programs/supervisor.orx"
cp build/programs/sysinit.orx "$TMP/jail/programs/sysinit.orx"
cp build/programs/shell.orx   "$TMP/jail/programs/shell.orx"
cp build/programs/login.orx   "$TMP/jail/programs/login.orx" 2>/dev/null || true

# Daemons.
SOCK="$TMP/bar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!; for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 --config tools/devices/oriscdir.default.conf > "$TMP/dir.out" 2>&1 &
DIR=$!; for _ in $(seq 50); do grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break; sleep 0.05; done
python3 tools/devices/hostfsd --directory-pid 18 --instance 0 --socket "$SOCK" --pid 17 --root "$TMP/jail" > "$TMP/hf.out" 2>&1 &
HF=$!; for _ in $(seq 50); do grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break; sleep 0.05; done

# Terminal CPU: pid 0, O8 = directory (18=1@9).  Runs termfw, which hands off to
# the co-resident supervisor.  Bounded by an alarm — the firmware idle-yields
# forever once the supervisor is up.
perl -e 'alarm shift; exec @ARGV' 30 python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" --service "18=1@9" --service "0=0@0" --service "0=0@0" \
    "$TMP/termfw.orx" > "$TMP/cpu0.out" 2>&1 || true

kill -KILL $HF $DIR $BAR 2>/dev/null || true
wait 2>/dev/null || true

echo "--- terminal CPU (termfw -> co-resident supervisor) ---"
cat "$TMP/cpu0.out"

grep -q "termfw: self-test PASS"          "$TMP/cpu0.out" || { echo "FAIL: no splash PASS" >&2; exit 1; }
grep -q "termfw: system software running" "$TMP/cpu0.out" || { echo "FAIL: firmware never handed off" >&2; exit 1; }
grep -q "supervisor: booting"             "$TMP/cpu0.out" || { echo "FAIL: supervisor didn't boot co-resident" >&2; exit 1; }
grep -q "sysinit: online"                 "$TMP/cpu0.out" || { echo "FAIL: co-resident spawn service didn't run sysinit" >&2; exit 1; }
if grep -q "FAIL:" "$TMP/cpu0.out"; then echo "FAIL: termfw reported a failure" >&2; exit 1; fi

echo "PASS"

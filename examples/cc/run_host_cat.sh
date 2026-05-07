#!/bin/sh
# run_host_cat.sh — multi-process demo of hostfsd. Spawns the
# crossbar, hostfsd (jailed to the repo root), and a CPU running
# host_cat.c that reads README.md via the host filesystem service.
#
# --service slot order: each --service spec lands at the next free
# O5..O15. host_io requires hostfsd's service ref in O10, so we
# pad with five service refs ahead of it (none of which the demo
# uses; they just consume slots).
#
#   O5..O9 = padding (service refs we don't use)
#   O10    = hostfsd (pid=17, idx=1)

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/host_cat.c \
    > "$TMP/program.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/program.i" > "$TMP/program.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/program.s"                  -o "$TMP/program.oro"
python3 tools/ld/orld -o "$TMP/host_cat.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/program.oro" \
    build/liborisc.ora

SOCK="$TMP/oriscbar.sock"

# 1) Crossbar.
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

# 2) hostfsd, jailed to the repo root.
python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$ROOT" -v \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

# 3) Demo CPU.
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "0=0@0" --service "17=1@9" \
    "$TMP/host_cat.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $CPU 2>/dev/null || true
kill -TERM $HF 2>/dev/null || true
wait $HF 2>/dev/null || true
kill -TERM $BAR 2>/dev/null || true
wait $BAR 2>/dev/null || true

cat "$TMP/cpu.out"

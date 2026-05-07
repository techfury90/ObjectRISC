#!/bin/sh
# run.sh — build and run the Dhrystone benchmark on Object RISC.
#
# Usage:
#   examples/cc/dhrystone/run.sh           # default DHRY_RUNS
#   DHRY_RUNS=20000 examples/cc/dhrystone/run.sh
#
# Builds dhry.c through pcc → asmorisc → orld and runs the resulting
# .orx under simorisc (single-CPU, in-process). Output is the
# benchmark's standard report — final globals, elapsed cycles +
# microseconds, and dhry/s + DMIPS at the OR-1000's nominal 16 / 20
# MHz clock rates from Vol I.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
DHRY_RUNS="${DHRY_RUNS:-5000}"

"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib \
    -DDHRY_RUNS=$DHRY_RUNS \
    examples/cc/dhrystone/dhry.c > "$TMP/dhry.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" \
    < "$TMP/dhry.i" > "$TMP/dhry.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/dhry.s"                     -o "$TMP/dhry.oro"
python3 tools/ld/orld -o "$TMP/dhry.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/dhry.oro" \
    build/liborisc.ora

# A high cycle ceiling — the benchmark loop does a lot of work per
# iteration. Default is plenty for DHRY_RUNS=5000; bump if needed.
exec python3 tools/sim/simorisc --max-cycles 200000000 "$TMP/dhry.orx"

#!/bin/sh
# test_fb_local_smoke.sh — exercise simorisc's TAG_FRAMEBUFFER
# primitive (Phase 60) end-to-end without any external services.
#
# Architecture under test: just simorisc running fb_local_smoke.orx
# standalone (no --connect, no oriscbar, no oriscterm).  The smoke
# allocates a 16×16 framebuffer locally via ObjAllocFramebuffer,
# stores a known pattern, fetches it back, and verifies byte-for-byte.
#
# Headless: no --display flag, so no Tk required (which is right for
# CI).  Interactive `make boot` flow will pass --display tk in a
# separate change once the WM rewires onto local framebuffers.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# Build fb_local_smoke.orx.
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/fb_local_smoke.c \
    > "$TMP/p.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/p.i" > "$TMP/p.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/p.s"                        -o "$TMP/p.oro"
python3 tools/ld/orld -o "$TMP/fb_local_smoke.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/p.oro" \
    build/liborisc.ora

# Run standalone — no socket, no peers.
python3 tools/sim/simorisc "$TMP/fb_local_smoke.orx" \
    > "$TMP/cpu.out" 2> "$TMP/cpu.err"

echo "--- fb_local_smoke stdout ---"
cat "$TMP/cpu.out"
echo "--- fb_local_smoke stderr ---"
cat "$TMP/cpu.err"

grep -q "fb_local_smoke: PASS" "$TMP/cpu.out" \
    || { echo "FAIL: smoke didn't reach PASS line" >&2; exit 1; }

if grep -q "FAIL:" "$TMP/cpu.out"; then
    echo "FAIL: smoke reported FAIL" >&2
    exit 1
fi

echo "PASS"

#!/bin/sh
# test_wm_fontopen.sh — the WM font_open(name) client API.
#
# Boots the WM against oriscdir (with the /fonts mount) + hostfsd, then runs a
# client (font_open_smoke) that exercises font_open:
#   - a BUILT-IN name ("luRS") resolves to its fixed id 0 (FONT_FACE_PROP);
#   - a NEW name ("luBI", Lucida Bold Italic — not a built-in face) loads into
#     a fresh dynamic slot and returns id 4;
#   - RE-OPEN of "luBI" returns the same id 4 (idempotent — no second load);
#   - a MISSING name ("nofont") replies a negative error.
# The client asserts all four and prints "font_open_smoke: PASS".
#
# This is the dispatch/id-assignment gate for font_open; test_wm_fontload
# already covers the underlying load+cache+blit path.
#
# Architecture: oriscbar + oriscdir(pid18,--config) + hostfsd(pid17,--root) +
# fake_terminal(pid16) + simorisc CPU0=oriscwm.orx + CPU1=font_open_smoke.orx.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
[ -f build/liborisc.ora ] || make -s lib >/dev/null
TMP=$(mktemp -d); trap "rm -rf $TMP" EXIT
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

build_orx() {  # $1 = source .c, $2 = output .orx
    "$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
        -I tools/cc/arch/orisc -I tools/cc/lib "$1" > "$TMP/p.i"
    "$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/p.i" > "$TMP/p.s"
    python3 tools/asm/asmorisc -r "$TMP/p.s" -o "$TMP/p.oro"
    python3 tools/ld/orld -o "$2" "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/p.oro" build/liborisc.ora
}
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
build_orx ouroboros/oriscwm.c          "$TMP/oriscwm.orx"
build_orx examples/cc/font_open_smoke.c "$TMP/fos.orx"

SOCK="$TMP/bar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 & BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done
python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 \
    --config tools/devices/oriscdir.default.conf -v > "$TMP/dir.out" 2>&1 & DIR=$!
for _ in $(seq 60); do grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break; sleep 0.05; done
python3 tools/devices/hostfsd --socket "$SOCK" --pid 17 --instance 0 \
    --directory-pid 18 --root "$ROOT" -v > "$TMP/hf.out" 2>&1 & HF=$!
for _ in $(seq 60); do grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break; sleep 0.05; done
python3 tools/devices/tests/fake_terminal.py --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 --linger 14.0 > "$TMP/term.out" 2>&1 & TERMP=$!
for _ in $(seq 60); do grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" --service "18=1@9" \
    "$TMP/oriscwm.orx" > "$TMP/wm.out" 2>&1 & WM=$!
for _ in $(seq 120); do grep -q "lutRS.wmf loaded\|failsafe" "$TMP/wm.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/sim/simorisc --connect "$SOCK" --pid 1 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" --service "18=1@9" \
    "$TMP/fos.orx" > "$TMP/cpu.out" 2>&1 & CPU=$!
wait $CPU 2>/dev/null || true
kill -TERM $WM $TERMP $HF $DIR $BAR 2>/dev/null || true
wait 2>/dev/null || true

if grep -q "font_open_smoke: PASS" "$TMP/cpu.out"; then
    echo "PASS: font_open resolved built-in/new/re-open/missing correctly"
    grep -i "font_open:" "$TMP/cpu.out" || true
    exit 0
fi
echo "FAIL: font_open client did not PASS"
echo "--- client ---"; grep -iE "font_open" "$TMP/cpu.out" || cat "$TMP/cpu.out"
echo "--- WM ---"; grep -iE "oriscwm:|font" "$TMP/wm.out" | tail -8 || true
exit 1

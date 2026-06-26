#!/bin/sh
# test_wm_fontload.sh — Phase B: the WM loads a font from /fonts at runtime.
#
# Boots the WM against oriscdir (with the canonical /fonts mount config) +
# hostfsd (jailed to the repo root, serving fonts/*.wmf). The WM discovers
# hostfsd by walking /sys/hostfsd/0, hf_init's, vfs_open's /fonts/luRS.wmf
# through the MOUNT, ObjAllocs a byte object, and streams the WMF1 blob in
# with hf_read_obj. Success is the WM's console line reporting the load.
#
# It also caches the WMF1 header + width table into the data segment (via the
# ObjFetchBytes-through-stack bounce) and self-checks that the cached widths
# match the baked blob — proving font_advance reads the LOADED widths, so the
# menu measures + renders with no reference to the compiled-in blob.
#
# This exercises the whole dynamic-font LOAD + width-cache path end-to-end (the
# render is framebuffer/Tk-only, eyeballed on `make boot`). It is also the guard
# that the WM↔hostfsd plumbing (O8/DIR_SLOT, O10 discovery, the obj_adopt_slot
# case for the font slot, the ObjFetchBytes cache) keeps working as Phase C
# migrates more faces.
#
# Architecture under test:
#   - oriscbar (crossbar)
#   - oriscdir  pid 18  (--config oriscdir.default.conf → /fonts mount)
#   - hostfsd   pid 17  (--root $ROOT → serves fonts/luRS.wmf, 3152 B)
#   - fake_terminal pid 16
#   - simorisc CPU 0 running oriscwm.orx (O8 = dir; hostfsd discovered)

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

[ -f build/liborisc.ora ] || make -s lib >/dev/null

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

# --- build the WM (.orx) from source -------------------------------
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib ouroboros/oriscwm.c > "$TMP/wm.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/wm.i" > "$TMP/wm.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/wm.s"                       -o "$TMP/wm.oro"
python3 tools/ld/orld -o "$TMP/oriscwm.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/wm.oro" build/liborisc.ora

SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/oriscdir --socket "$SOCK" --pid 18 \
    --config tools/devices/oriscdir.default.conf -v > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 60); do grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/devices/hostfsd --socket "$SOCK" --pid 17 --instance 0 \
    --directory-pid 18 --root "$ROOT" -v > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 60); do grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/devices/tests/fake_terminal.py --socket "$SOCK" --pid 16 \
    --directory-pid 18 --instance 0 --linger 12.0 > "$TMP/term.out" 2>&1 &
TERMP=$!
for _ in $(seq 60); do grep -q "fake_terminal READY" "$TMP/term.out" 2>/dev/null && break; sleep 0.05; done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" --service "18=1@9" \
    "$TMP/oriscwm.orx" > "$TMP/wm.out" 2>&1 &
WM=$!
# luBS loads second; wait for it (or any failure) so both faces are settled.
for _ in $(seq 200); do
    grep -q "luBS.wmf loaded\|MISMATCH\|load failed\|hostfsd unavailable\|fetch failed" "$TMP/wm.out" 2>/dev/null && break
    sleep 0.05
done

kill -TERM $WM $TERMP $HF $DIR $BAR 2>/dev/null || true
wait 2>/dev/null || true

# Success = BOTH chrome faces (luRS menu, luBS titles) loaded (3152 B) and their
# cached width tables validated against the baked blobs.
if grep -q "/fonts/luRS.wmf loaded (3152 B); widths OK" "$TMP/wm.out" && \
   grep -q "/fonts/luBS.wmf loaded (3152 B); widths OK" "$TMP/wm.out"; then
    echo "PASS: WM loaded + cache-validated luRS + luBS from /fonts"
    exit 0
fi
echo "FAIL: WM did not load + cache luRS + luBS from /fonts"
echo "--- wm.out (oriscwm lines) ---"; grep -iE "oriscwm:|font" "$TMP/wm.out" || true
echo "--- dir.out (tail) ---"; tail -5 "$TMP/dir.out" || true
echo "--- hf.out (tail) ---"; tail -5 "$TMP/hf.out" || true
exit 1

#!/bin/sh
# test_rtcview.sh — build-check for the RTC render app
# (ouroboros/programs/rtcview.c): it compiles + links against liborisc.
#
# rtcview is a shell/menu-spawned WM viewer (wm_open_session + term_init +
# vec_text, mdview's twin) — like mdview it has no headless run test; it is
# verified by running it. Its render DATA path (Document -> rtc_layout ->
# display list) is covered headlessly by test_rtc_doc.sh / test_rtc_layout.sh;
# this guards that the app itself keeps building.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then make -s lib >/dev/null; fi

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"
if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    echo "SKIP: pcc not built at $PCC_BUILD (run tools/cc/build.sh)" >&2
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib ouroboros/programs/rtcview.c > "$TMP/rtcv.i"
"$CCOM" < "$TMP/rtcv.i" > "$TMP/rtcv.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s       -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/rtcv.s"                    -o "$TMP/rtcv.oro"
python3 tools/ld/orld -o "$TMP/rtcview.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/rtcv.oro" build/liborisc.ora

[ -s "$TMP/rtcview.orx" ] \
    || { echo "FAIL: rtcview.orx did not build" >&2; exit 1; }

echo "PASS"

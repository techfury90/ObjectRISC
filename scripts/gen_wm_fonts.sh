#!/bin/sh
# gen_wm_fonts.sh — (re)bake the WM's OPEN LOOK font faces.
#
# Bakes three self-describing WM font objects out of the OpenLook CD-ROM
# BDF sources (tools/bdf.py + tools/gen_wm_font.py --face):
#
#   font_luRS   Lucida Sans, proportional   — title bars / menus  (chrome)
#   font_lutRS  Lucida Sans Typewriter, mono — body face, re-baked in the
#                                              self-describing format
#   font_olgl   OPEN LOOK glyph font        — pushpins, menu marks, rings,
#                                              check boxes, resize corners,
#                                              scrollbar arrows (used by
#                                              later chrome PRs)
#
# luRS + lutRS land in ouroboros/wm_fonts.h (included by oriscwm.c).  olgl
# lands in ouroboros/wm_fonts_olgl.h — committed and available, but not yet
# #included anywhere (the chrome PR that consumes the glyphs pulls it in).
#
# The BDF root defaults to the local OpenLook CD-ROM checkout; override with
#   OPENLOOK_BDF=/path/to/fonts/bdf sh scripts/gen_wm_fonts.sh
#
# Licensing: the faces carry the Sun (c) 1989 + Bigelow & Holmes (c) 1985,
# 1986 notices (baked into each header comment by the generator).  See the
# project NOTICE.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OPENLOOK_BDF=${OPENLOOK_BDF:-/Users/lando/Downloads/OpenLookCDROM-master/src/lib/xview3.2p1-X11R6-LinuxElf/fonts/bdf}
GEN="python3 tools/gen_wm_font.py"

if [ ! -d "$OPENLOOK_BDF" ]; then
    echo "OpenLook BDF dir not found: $OPENLOOK_BDF" >&2
    echo "set OPENLOOK_BDF=/path/to/fonts/bdf" >&2
    exit 1
fi

BANNER='/* wm_fonts.h — GENERATED.  DO NOT EDIT.  Regenerate: sh scripts/gen_wm_fonts.sh
 *
 * The lutRS monospace face is the WM'"'"'s single compiled-in FAILSAFE font.  The
 * WM loads all four faces (luRS / luBS / olgl / lutRS) from /fonts at runtime
 * and falls back to this baked lutRS for any face whose load fails.  The other
 * three faces are NOT baked in — they live only as fonts/*.wmf.
 *
 * A wm_font_t descriptor over a uint32 blob the WM hands to the extended
 * ObjBlitGlyphs path (R5 bit 31).  #included after wm_font_t is defined.
 *
 * Font assets (c) 1989 Sun Microsystems and (c) 1985, 1986 Bigelow & Holmes;
 * Lucida is a B&H trademark.  See the project NOTICE.
 */'

OUT="ouroboros/wm_fonts.h"
{
    echo "$BANNER"
    echo
    $GEN --face lutRS --bdf "$OPENLOOK_BDF/100dpi/lutRS10.bdf" \
         --base 32 --count 95 --cell 8x16
} > "$OUT"
echo "wrote $OUT"

# On-disk .wmf font objects for dynamic font loading.  These are the SAME
# blobs baked into the headers above, written as raw WMF1 bytes so the WM can
# load a face from /fonts at runtime (served by hostfsd, jailed to the repo
# root) instead of compiling it in.  Committed (like the headers) because they
# can't be rebuilt without the OpenLook BDF sources, which aren't in the repo.
FONTS_DIR="fonts"
mkdir -p "$FONTS_DIR"
$GEN --out "$FONTS_DIR/luRS.wmf"  --bdf "$OPENLOOK_BDF/75dpi/luRS12.bdf" \
     --base 32 --count 95 --cell 12x16 --proportional
$GEN --out "$FONTS_DIR/luBS.wmf"  --bdf "$OPENLOOK_BDF/75dpi/luBS12.bdf" \
     --base 32 --count 95 --cell 12x16 --proportional
$GEN --out "$FONTS_DIR/lutRS.wmf" --bdf "$OPENLOOK_BDF/100dpi/lutRS10.bdf" \
     --base 32 --count 95 --cell 8x16
# count 200 (codepoints 19..218) so the scrollbar dimple/box glyphs (cp 194-198)
# + the horizontal-scrollbar twins (cp 200/201) are included, not just 19..185.
# Force --cell 47x47 (the original count-167 auto-fit) so the existing chrome
# glyphs (19..185) stay byte-identical; an unused taller glyph in 186..218 is
# clipped (the scrollbar glyphs all fit 47x47, verified by the headless render).
$GEN --out "$FONTS_DIR/olgl.wmf"  --bdf "$OPENLOOK_BDF/misc/olgl12.bdf" \
     --base 19 --count 200 --cell 47x47
# luBI (Lucida Bold Italic) is NOT a built-in WM face — it's the demo font for
# font_open(): a client can load it by name at runtime (test_wm_fontopen).
$GEN --out "$FONTS_DIR/luBI.wmf"  --bdf "$OPENLOOK_BDF/75dpi/lubI12.bdf" \
     --base 32 --count 95 --cell 12x16 --proportional
echo "wrote $FONTS_DIR/{luRS,luBS,lutRS,olgl,luBI}.wmf"

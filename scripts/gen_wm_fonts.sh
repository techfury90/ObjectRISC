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

BANNER='/* wm_fonts.h — GENERATED self-describing WM font faces.  DO NOT EDIT.
 *
 * Regenerate with:  sh scripts/gen_wm_fonts.sh
 *
 * Each face is a wm_font_t descriptor over a uint32 blob the WM hands to
 * the extended ObjBlitGlyphs firmware path (R5 bit 31).  This header must
 * be #included AFTER wm_font_t is defined (it is, in oriscwm.c).
 *
 * Font assets are (c) 1989 Sun Microsystems and (c) 1985, 1986 Bigelow &
 * Holmes; Lucida is a B&H trademark.  See the project NOTICE.
 */'

OUT="ouroboros/wm_fonts.h"
{
    echo "$BANNER"
    echo
    $GEN --face luRS  --bdf "$OPENLOOK_BDF/75dpi/luRS12.bdf" \
         --base 32 --count 95 --cell 12x16 --proportional
    echo
    $GEN --face lutRS --bdf "$OPENLOOK_BDF/100dpi/lutRS10.bdf" \
         --base 32 --count 95 --cell 8x16
} > "$OUT"
echo "wrote $OUT"

OLGL_BANNER='/* wm_fonts_olgl.h — GENERATED OPEN LOOK glyph font face.  DO NOT EDIT.
 *
 * Regenerate with:  sh scripts/gen_wm_fonts.sh
 *
 * The OPEN LOOK glyph font (pushpins, menu marks, default rings, check
 * boxes, 2D menu pins, resize corners, scrollbar arrows).  Auto-fit cell
 * (the scrollbar cable/elevator glyphs span the full 47x47 envelope).
 * Baked and committed so the chrome PRs that consume these glyphs can
 * #include it; oriscwm.c does not yet pull it in.
 *
 * Font assets (c) 1989 Sun Microsystems.  See the project NOTICE.
 */'

OUT_OLGL="ouroboros/wm_fonts_olgl.h"
{
    echo "$OLGL_BANNER"
    echo
    $GEN --face olgl --bdf "$OPENLOOK_BDF/misc/olgl12.bdf" \
         --base 19 --count 167
} > "$OUT_OLGL"
echo "wrote $OUT_OLGL"

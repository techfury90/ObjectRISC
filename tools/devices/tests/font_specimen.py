#!/usr/bin/env python3
"""font_specimen.py — render the font_demo specimen headless.

font_demo.c draws proportional Lucida (luRS), monospace Lucida Typewriter
(lutRS) and the OPEN LOOK glyph face (olgl) into a window through the new
client font service (VEC_OP_TEXT_*).  The live framebuffer is only capturable
under the Tk display (`make boot` + Cmd+S), so this mirror renders the SAME
content through the SAME firmware primitive (the EXTENDED ObjBlitGlyphs path),
using the exact faces the WM bakes (scripts/gen_wm_fonts.sh), and writes a PPM
so the specimen can be eyeballed without a display.

Usage:  python3 tools/devices/tests/font_specimen.py   # -> /tmp/orisc-font-specimen.ppm
"""
import importlib.machinery
import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
_loader = importlib.machinery.SourceFileLoader("simorisc", str(ROOT / "tools" / "sim" / "simorisc"))
sim = importlib.util.module_from_spec(importlib.util.spec_from_loader("simorisc", _loader))
_loader.exec_module(sim)
from gen_wm_font import face_blob   # noqa: E402

BDF = ("/Users/lando/Downloads/OpenLookCDROM-master/src/lib/"
       "xview3.2p1-X11R6-LinuxElf/fonts/bdf")
PID = 7

# OPEN LOOK palette indices (match VEC_PALETTE_HEX / font_demo.c).
PAPER, INK, LABEL, RULE = 11, 14, 13, 12


def _desc(storage, type_tag=0, fb_w=0, fb_h=0):
    return SimpleNamespace(live=True, generation=1, type_tag=type_tag,
                           storage=storage, length=len(storage),
                           fb_width=fb_w, fb_height=fb_h, fb_dirty=False)


def _ref(index, caps):
    return sim.make_ref(generation=1, home=PID, index=index, caps=caps)


class FakeCPU:
    def __init__(self, descs):
        self.pid = PID; self.descriptors = descs
        self._opr = {}; self._gpr = {}
    def get_opr(self, i): return self._opr.get(i, 0)
    def set_opr(self, i, v): self._opr[i] = v
    def get_gpr(self, i): return self._gpr.get(i, 0)
    def set_gpr(self, i, v): self._gpr[i] = v


def main():
    # The three faces, baked exactly as scripts/gen_wm_fonts.sh does.
    luRS,  _ = face_blob(f"{BDF}/75dpi/luRS12.bdf",   32, 95, (12, 16), True)
    luBS,  _ = face_blob(f"{BDF}/75dpi/luBS12.bdf",   32, 95, (12, 16), True)
    lutRS, _ = face_blob(f"{BDF}/100dpi/lutRS10.bdf", 32, 95, (8, 16),  False)
    olgl,  _ = face_blob(f"{BDF}/misc/olgl12.bdf",    19, 167, (47, 47), False)

    F_LURS, F_LUBS, F_LUTRS, F_OLGL, F_TEXT = 2, 3, 4, 5, 6
    W, H = 680, 244
    fb = bytearray([PAPER] * W * H)
    descs = [None,
             _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=W, fb_h=H),
             _desc(bytearray(luRS)),
             _desc(bytearray(luBS)),
             _desc(bytearray(lutRS)),
             _desc(bytearray(olgl)),
             _desc(bytearray(b""))]   # F_TEXT, rebound per blit

    def cpu0():
        c = FakeCPU(descs); c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W)); return c

    def fill(x, y, w, h, color):
        c = cpu0()
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, ((w & 0xFFFF) << 16) | (h & 0xFFFF))
        c.set_gpr(6, color)
        sim.primitive_ObjFillRect(c)

    def text(face_idx, x, y, s, fg):
        b = s if isinstance(s, (bytes, bytearray)) else s.encode()
        descs[F_TEXT] = _desc(bytearray(b))
        c = cpu0()
        c.set_opr(2, _ref(face_idx, sim.CAP_R))
        c.set_opr(3, _ref(F_TEXT, sim.CAP_R))
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        # EXTENDED (bit31) + transparent (bit30) | len<<16 | fg<<8 | bg
        c.set_gpr(5, 0x80000000 | 0x40000000 | (len(b) << 16) | (fg << 8) | PAPER)
        c.set_gpr(6, 0); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    # --- the specimen (mirrors font_demo.c's layout) -----------------------
    text(F_LURS, 8, 6, "Object RISC -- OPEN LOOK Font Specimen", INK)
    fill(8, 26, W - 16, 1, RULE)

    text(F_LURS, 8, 38, "Lucida Sans (luRS, proportional):", LABEL)
    text(F_LURS, 16, 56,
         "The quick brown fox jumps over the lazy dog 0123456789", INK)

    text(F_LURS, 8, 84, "Lucida Sans Bold (luBS, proportional):", LABEL)
    text(F_LUBS, 16, 102,
         "The quick brown fox jumps over the lazy dog 0123456789", INK)

    text(F_LURS, 8, 130, "Lucida Typewriter (lutRS, monospace):", LABEL)
    text(F_LUTRS, 16, 148, "$ ls -la *.orx | grep ouroboros", INK)

    text(F_LURS, 8, 176, "OPEN LOOK glyphs (olgl):", LABEL)
    for i, cp in enumerate((19, 20, 21, 22, 23, 47)):
        text(F_OLGL, 16 + i * 28, 194, bytes([cp]), INK)

    lut = sim._build_palette_lut()
    rgb = b"".join(lut[bb] for bb in fb)
    out = "/tmp/orisc-font-specimen.ppm"
    with open(out, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (W, H))
        fh.write(rgb)
    print(f"wrote {out}  ({W}x{H})")


if __name__ == "__main__":
    main()

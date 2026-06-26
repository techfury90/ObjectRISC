#!/usr/bin/env python3
"""menu_capsule.py — render the OPEN LOOK desktop-menu capsule headless.

oriscwm draws the highlighted menu item as a recessed OPEN LOOK capsule:
a BG2 body with BG3 shadow on the top edge + upper rounded endcaps and a
WHITE highlight on the bottom edge + lower endcaps.  The rounded endcaps
are olgl button glyphs (enc 24/25/26 left, 28/27/29 right) blitted in three
colour passes (olgx_draw_button's recipe); the middle is a flat BG2 fill
with 1px BG3/WHITE edge lines.

The live framebuffer is Tk-only, so this mirror drives the SAME firmware
primitive (EXTENDED ObjBlitGlyphs) against the SAME baked olgl face the WM
uses (scripts/gen_wm_fonts.sh) and writes a 4x-zoomed PNG so the endcap
arcs can be eyeballed without a display — the headless twin of right-
clicking the desktop in `make boot`.

Usage:  python3 tools/devices/tests/menu_capsule.py   # -> /tmp/orisc-menu-capsule.png
"""
import importlib.machinery
import importlib.util
import struct
import sys
import zlib
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
# OPEN LOOK gray group (match VEC_PALETTE_HEX / oriscwm WM_FACE_*).
WHITE, BG1, BG2, BG3, BLACK, BLUE = 11, 10, 12, 13, 14, 9
ENDCAP = 9            # MENU_ENDCAP_PX
ITEM_H = 20           # MENU_ITEM_H_PX


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
    luRS, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    olgl, _ = face_blob(f"{BDF}/misc/olgl12.bdf", 19, 167, (47, 47), False)
    F_LURS, F_OLGL, F_TEXT = 2, 4, 5
    W, H = 150, 110
    fb = bytearray([BLUE] * W * H)
    descs = [None,
             _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=W, fb_h=H),
             _desc(bytearray(luRS)), None,
             _desc(bytearray(olgl)),
             _desc(bytearray(b""))]

    def cpu0():
        c = FakeCPU(descs); c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W)); return c

    def fill(x, y, w, h, color):
        if w <= 0 or h <= 0:
            return
        c = cpu0()
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, ((w & 0xFFFF) << 16) | (h & 0xFFFF))
        c.set_gpr(6, color)
        sim.primitive_ObjFillRect(c)

    def blit(face_idx, x, y, s, fg, bg):
        b = s if isinstance(s, (bytes, bytearray)) else s.encode()
        descs[F_TEXT] = _desc(bytearray(b))
        c = cpu0()
        c.set_opr(2, _ref(face_idx, sim.CAP_R))
        c.set_opr(3, _ref(F_TEXT, sim.CAP_R))
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, 0x80000000 | 0x40000000 | (len(b) << 16) | (fg << 8) | bg)
        c.set_gpr(6, 0); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def capsule(x, y, w):
        mx, mw, rx = x + ENDCAP, w - 2 * ENDCAP, x + w - ENDCAP
        fill(mx, y, mw, ITEM_H, BG2)                       # fill pass
        blit(F_OLGL, x, y, bytes([26]), BG2, BG1)
        blit(F_OLGL, rx, y, bytes([29]), BG2, BG1)
        fill(mx, y, mw, 1, BG3)                            # top pass
        blit(F_OLGL, x, y, bytes([24]), BG3, BG1)
        blit(F_OLGL, rx, y, bytes([28]), BG3, BG1)
        fill(mx, y + ITEM_H - 2, mw, 1, WHITE)             # bottom pass (aligns
        #                                  with the endcap lower arcs, 1px up)
        blit(F_OLGL, x, y, bytes([25]), WHITE, BG1)
        blit(F_OLGL, rx, y, bytes([27]), WHITE, BG1)

    # BG1 plate + 1px raised frame, three items, the middle one highlighted.
    mx, my, mw, mh = 20, 16, 110, 64
    fill(mx, my, mw, mh, BG1)
    fill(mx, my, mw, 1, WHITE); fill(mx, my, 1, mh, WHITE)
    fill(mx, my + mh - 1, mw, 1, BG3); fill(mx + mw - 1, my, 1, mh, BG3)
    for i, (name, hi) in enumerate((("Shell", False),
                                    ("Mouse Paint", True),
                                    ("Edit", False))):
        iy, ix, iw = my + 2 + i * ITEM_H, mx + 2, mw - 4
        fill(ix, iy, iw, ITEM_H, BG1)
        if hi:
            capsule(ix, iy, iw)
        blit(F_LURS, ix + ENDCAP, iy + 2, name, BLACK, BG2 if hi else BG1)

    lut = sim._build_palette_lut()
    Z = 4
    rows = bytearray()
    for y in range(H):
        scan = bytearray()
        for x in range(W):
            scan += bytes(lut[fb[y * W + x]]) * Z
        for _ in range(Z):
            rows.append(0); rows += scan

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    out = "/tmp/orisc-menu-capsule.png"
    with open(out, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W * Z, H * Z, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        fh.write(chunk(b"IEND", b""))
    print(f"wrote {out}  ({W * Z}x{H * Z})")


if __name__ == "__main__":
    main()

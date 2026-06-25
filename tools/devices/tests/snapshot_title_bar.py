#!/usr/bin/env python3
"""snapshot_title_bar.py — render a WM title bar exactly as paint_title_bar
composes it (OPEN LOOK olwm look), driving the REAL simorisc firmware ops,
and write a PPM.

This is the headless companion to the WM's Cmd+S framebuffer snapshot: it
reproduces oriscwm.c's title-bar layout (its own #define constants, mirrored
below) and paints it with the same firmware primitives the WM uses —

  * ObjFillRect    (#0x10D)  the flat BG1 bar base + 1px black separator
  * ObjBlitGlyphs  (#0x10C, EXTENDED)  the raised menu button's ▽ mark
                                       (olgl cp 22) and the proportional
                                       Lucida Sans title (luRS)
  * the recessed focus stripe / raised menu button via ObjFillRect bevels

so the pixels are produced by the shipping firmware, not a re-implementation.
Renders a FOCUSED bar by default (pass `--unfocused` for the flat variant).

Usage:  python3 tools/devices/tests/snapshot_title_bar.py ["Window Title"] [--unfocused]
Writes /tmp/orisc-title-bar.ppm.
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

# --- oriscwm.c title-bar layout constants (kept in sync by hand) -----------
CELL_W, CELL_H = 8, 16
N_COLS = 80
BORDER_LINE_PX = 2
TITLE_BAR_PX = 24
CELL_AREA_W_PX = CELL_W * N_COLS            # 640
USABLE_W_PX = (N_COLS + 2) * CELL_W         # 656
TITLE_X_OFF_PX = CELL_W                      # 8
TITLE_Y_OFF_PX = BORDER_LINE_PX             # 2 (flush under the 2px border)
TITLE_TEXT_Y_OFF_PX = TITLE_Y_OFF_PX + (TITLE_BAR_PX - CELL_H) // 2   # 6
MENU_BTN_W = TITLE_BAR_PX                    # 24
OL_MENU_MARK_CP = 22
OL_MENU_MARK_W, OL_MENU_MARK_H = 16, 15
BEVEL_RAISED, BEVEL_FILL = 0x01, 0x02

# palette indices
BG1, WHITE_I, BG2, BG3, BLACK = 10, 11, 12, 13, 14
WM_TITLE_TEXT_FG = BLACK


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
    argv = [a for a in sys.argv[1:] if a != "--unfocused"]
    focused = "--unfocused" not in sys.argv
    title = (argv[0] if argv else "Object RISC Shell").encode()

    luRS, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    olgl, _ = face_blob(f"{BDF}/misc/olgl12.bdf", 19, 167, (47, 47), False)

    F_LURS, F_TEXT, F_OLGL = 2, 3, 4
    fb_w = USABLE_W_PX
    fb_h = TITLE_Y_OFF_PX + TITLE_BAR_PX + 1 + 4   # border + bar + separator + pad
    fb = bytearray([BG1] * fb_w * fb_h)
    descs = [None,
             _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=fb_w, fb_h=fb_h),
             _desc(bytearray(luRS)),          # F_LURS
             _desc(bytearray(b"")),           # F_TEXT (rebound per call)
             _desc(bytearray(olgl))]          # F_OLGL

    def cpu0():
        c = FakeCPU(descs); c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W)); return c

    def cpu_font(font_idx, text):
        descs[F_TEXT] = _desc(bytearray(text))
        c = cpu0(); c.set_opr(2, _ref(font_idx, sim.CAP_R))
        c.set_opr(3, _ref(F_TEXT, sim.CAP_R)); return c

    def fill(x, y, w, h, color):
        c = cpu0()
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, ((w & 0xFFFF) << 16) | (h & 0xFFFF))
        c.set_gpr(6, color)
        sim.primitive_ObjFillRect(c)

    def bevel_box(x, y, w, h, mode):
        e = 2
        raised = mode & BEVEL_RAISED
        hi = WHITE_I if raised else BG3
        lo = BG3 if raised else WHITE_I
        if mode & BEVEL_FILL:
            fill(x, y, w, h, BG1 if raised else BG2)
        fill(x, y, w, e, hi)
        fill(x, y, e, h, hi)
        fill(x, y + h - e, w, e, lo)
        fill(x + w - e, y, e, h, lo)

    def blit(font_idx, x, y, text, fg, bg):
        c = cpu_font(font_idx, text)
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, 0x80000000 | 0x40000000 | (len(text) << 16) | (fg << 8) | bg)
        c.set_gpr(6, 0); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def adv(cp):
        gi = cp - 32
        return luRS[16 + gi] if 0 <= gi < 95 else 12

    # 0) 2px black top border (the WM chrome).
    fill(0, 0, fb_w, BORDER_LINE_PX, BLACK)

    # 1) Bar base: flat BG1, both focus states.
    fill(TITLE_X_OFF_PX, TITLE_Y_OFF_PX, CELL_AREA_W_PX, TITLE_BAR_PX, BG1)

    span_x = TITLE_X_OFF_PX + MENU_BTN_W
    span_w = CELL_AREA_W_PX - MENU_BTN_W

    # 2) Focused: recessed stripe (PRESSED bevel) across the title-text area.
    if focused:
        bevel_box(span_x, TITLE_Y_OFF_PX, span_w, TITLE_BAR_PX, BEVEL_FILL)

    # 3) Raised window-menu button + ▽ glyph (olgl cp 22).
    bevel_box(TITLE_X_OFF_PX, TITLE_Y_OFF_PX, MENU_BTN_W, TITLE_BAR_PX,
              BEVEL_RAISED | BEVEL_FILL)
    gx = TITLE_X_OFF_PX + (MENU_BTN_W - OL_MENU_MARK_W) // 2
    gy = TITLE_Y_OFF_PX + (TITLE_BAR_PX - OL_MENU_MARK_H) // 2
    blit(F_OLGL, gx, gy, bytes([OL_MENU_MARK_CP]), WM_TITLE_TEXT_FG, BG1)

    # 4) Centre + draw the proportional title (extended ObjBlitGlyphs).
    tpx = 0; n = 0
    while n < len(title) and tpx + adv(title[n]) <= span_w:
        tpx += adv(title[n]); n += 1
    start_x = span_x + (span_w - tpx) // 2
    text_bg = BG2 if focused else BG1
    blit(F_LURS, start_x, TITLE_TEXT_Y_OFF_PX, title[:n], WM_TITLE_TEXT_FG, text_bg)

    # 5) 1px black separator directly below the bar.
    fill(TITLE_X_OFF_PX, TITLE_Y_OFF_PX + TITLE_BAR_PX, CELL_AREA_W_PX, 1, BLACK)

    lut = sim._build_palette_lut()
    rgb = b"".join(lut[b] for b in fb)
    out = "/tmp/orisc-title-bar.ppm"
    with open(out, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (fb_w, fb_h))
        fh.write(rgb)
    print(f"wrote {out}  ({fb_w}x{fb_h}); {'focused' if focused else 'unfocused'} "
          f"title '{title.decode()}' {n} glyphs, {tpx}px, start_x={start_x}")


if __name__ == "__main__":
    main()

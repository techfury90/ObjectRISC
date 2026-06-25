#!/usr/bin/env python3
"""snapshot_desktop.py — render the WM desktop with the OPEN LOOK gray-on-blue
palette, driving the REAL simorisc firmware ops + the REAL palette LUT, and
pixel-check the key colors against the spec.

Mirrors oriscwm.c's paint sequence (paint_window_chrome -> paint_window_face ->
paint_title_bar -> paint_window_border) and its layout #defines, painting into a
single screen framebuffer with ObjFillRect (#0x10D) + ObjBlitGlyphs (#0x10C).
The colors come from simorisc's VEC_PALETTE_HEX, so what this renders is exactly
what the live WM composites.

Renders both the OLD dev palette and the NEW OPEN LOOK palette (before/after) so
the recolor is visible side by side.  Writes /tmp/orisc-desktop-{before,after}.ppm
and asserts the OPEN LOOK colors.

Usage:  python3 tools/devices/tests/snapshot_desktop.py
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

# oriscwm.c layout #defines
CELL_W, CELL_H = 8, 16
N_COLS = 80
BORDER_LINE_PX = 2
TITLE_BAR_PX = CELL_H
CELL_AREA_W_PX = CELL_W * N_COLS                       # 640
USABLE_W_PX = (N_COLS + 2) * CELL_W                    # 656
TITLE_X_OFF_PX = CELL_W                                # 8
TITLE_Y_OFF_PX = CELL_H                                # 16
TITLE_CELL_X_OFF = TITLE_CELL_Y_OFF = 1
CLOSE_BOX_CELLS = 3
CONTENT_X_OFF_PX = CELL_W                              # 8
CONTENT_Y_OFF_PX = (1 + 1) * CELL_H                    # 32 (border + title)
USABLE_H_PX = (2 + 1 + 8) * CELL_H                     # short window: 8 content rows
CELL_AREA_H_PX = 8 * CELL_H

# Palette index schemes (must match simorisc VEC_PALETTE_HEX + oriscwm.c).
NEW = dict(workspace=9, face=10, content=0, border=13,
           title_focus=8, title_unfocus=10, title_text=14,
           body_fg=1, body_bg=0)
OLD = dict(workspace=0, face=0, content=0, border=1,
           title_focus=8, title_unfocus=1, title_text=0,
           body_fg=1, body_bg=0)


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


class Scene:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.fb = bytearray(w * h)
        luRS, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
        lutRS, _ = face_blob(f"{BDF}/100dpi/lutRS10.bdf", 32, 95, (8, 16), False)
        self.luRS = luRS
        self.lutRS = lutRS
        self.lutRS_bitmaps_off = 16 + ((95 + 3) & ~3)
        self.descs = [None,
                      _desc(self.fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=w, fb_h=h),
                      _desc(bytearray(luRS)),       # idx2 luRS
                      _desc(bytearray(b"")),        # idx3 text (rebound per call)
                      _desc(bytearray(lutRS))]      # idx4 lutRS

    def _cpu(self, font_idx=None, text=None):
        if text is not None:
            self.descs[3] = _desc(bytearray(text))
        c = FakeCPU(self.descs)
        c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
        if font_idx:
            c.set_opr(2, _ref(font_idx, sim.CAP_R))
            c.set_opr(3, _ref(3, sim.CAP_R))
        return c

    def fill(self, x, y, w, h, color):
        c = self._cpu()
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, ((w & 0xFFFF) << 16) | (h & 0xFFFF))
        c.set_gpr(6, color)
        sim.primitive_ObjFillRect(c)

    def adv_luRS(self, cp):
        gi = cp - 32
        return self.luRS[16 + gi] if 0 <= gi < 95 else 12

    def draw_title(self, x, y, text, fg, bg):
        # proportional luRS, transparent over the bar
        c = self._cpu(font_idx=2, text=text)
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, 0x80000000 | 0x40000000 | (len(text) << 16) | (fg << 8) | bg)
        c.set_gpr(6, 0); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def draw_cells(self, cell_x, cell_y, text, fg, bg):
        # legacy 8x16 mono via the lutRS bitmap region
        c = self._cpu(font_idx=4, text=text)
        c.set_gpr(4, ((cell_x & 0xFFFF) << 16) | (cell_y & 0xFFFF))
        c.set_gpr(5, (len(text) << 16) | (fg << 8) | bg)
        c.set_gpr(6, self.lutRS_bitmaps_off); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def window(self, wx, wy, title, focused, scheme, body_lines):
        """Mirror handle_new_window: face -> content -> title bar -> border."""
        # paint_window_face: gray face, then content area navy
        self.fill(wx, wy, USABLE_W_PX, USABLE_H_PX, scheme["face"])
        self.fill(wx + CONTENT_X_OFF_PX, wy + CONTENT_Y_OFF_PX,
                  CELL_AREA_W_PX, CELL_AREA_H_PX, scheme["content"])
        # title bar
        bar_bg = scheme["title_focus"] if focused else scheme["title_unfocus"]
        self.fill(wx + TITLE_X_OFF_PX, wy + TITLE_Y_OFF_PX,
                  CELL_AREA_W_PX, TITLE_BAR_PX, bar_bg)
        avail = (N_COLS - CLOSE_BOX_CELLS) * CELL_W
        tb = title.encode()
        tpx = 0; n = 0
        while n < len(tb) and tpx + self.adv_luRS(tb[n]) <= avail:
            tpx += self.adv_luRS(tb[n]); n += 1
        sx = wx + TITLE_X_OFF_PX + (avail - tpx) // 2
        self.draw_title(sx, wy + TITLE_Y_OFF_PX, tb[:n], scheme["title_text"], bar_bg)
        box_col = (wx // CELL_W) + TITLE_CELL_X_OFF + (N_COLS - CLOSE_BOX_CELLS)
        self.draw_cells(box_col, (wy // CELL_H) + TITLE_CELL_Y_OFF,
                        b"[X]", scheme["title_text"], bar_bg)
        # console body text (legacy mono, gray on navy)
        for i, line in enumerate(body_lines):
            self.draw_cells((wx + CONTENT_X_OFF_PX) // CELL_W,
                            (wy + CONTENT_Y_OFF_PX) // CELL_H + i,
                            line.encode(), scheme["body_fg"], scheme["content"])
        # border (flat BG3 lines)
        b = scheme["border"]
        self.fill(wx, wy, USABLE_W_PX, BORDER_LINE_PX, b)
        self.fill(wx, wy + USABLE_H_PX - BORDER_LINE_PX, USABLE_W_PX, BORDER_LINE_PX, b)
        self.fill(wx, wy, BORDER_LINE_PX, USABLE_H_PX, b)
        self.fill(wx + USABLE_W_PX - BORDER_LINE_PX, wy, BORDER_LINE_PX, USABLE_H_PX, b)

    def save(self, path):
        lut = sim._build_palette_lut()
        rgb = b"".join(lut[bb] for bb in self.fb)
        with open(path, "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (self.w, self.h))
            fh.write(rgb)

    def px(self, x, y):
        lut = sim._build_palette_lut()
        return tuple(lut[self.fb[y * self.w + x]])


def render(scheme, path):
    W, H = 760, 540
    s = Scene(W, H)
    s.fill(0, 0, W, H, scheme["workspace"])
    s.window(40, 40, "Object RISC Shell", True, scheme,
             ["$ ls", "hello.orx  shell.orx", "$ "])
    s.window(96, 300, "ps", False, scheme, ["PID  CMD", "  1  shell"])
    s.save(path)
    return s


if __name__ == "__main__":
    render(OLD, "/tmp/orisc-desktop-before.ppm")
    s = render(NEW, "/tmp/orisc-desktop-after.ppm")
    # pixel-check the OPEN LOOK colors against the spec / reference.
    # Focused window: wx=40 wy=40; border y/x[40,42); face margin; title
    # bar y[56,72); content below.  Sample clear (non-text) spots.
    ws = s.px(10, 10)               # workspace desktop
    face = s.px(44, 46)             # left/top face margin (BG1)
    titlebar = s.px(600, 62)        # focused title bar, right of the title text
    border = s.px(40, 100)          # left border line
    uf = s.px(600, 322)             # unfocused 'ps' title bar (wy=300 -> bar y[316,332))
    assert ws == (64, 160, 192), f"workspace {ws} != #40a0c0"
    assert titlebar == (255, 255, 255), f"focused title bar {titlebar} != #ffffff"
    assert face == (204, 204, 204), f"window face {face} != #cccccc"
    assert border == (102, 102, 102), f"border {border} != #666666"
    assert uf == (204, 204, 204), f"unfocused title {uf} != #cccccc"
    print("desktop colors OK: workspace=#40a0c0 titlebar=#ffffff "
          "face=#cccccc border=#666666 unfocused-title=#cccccc")
    print("wrote /tmp/orisc-desktop-before.ppm and /tmp/orisc-desktop-after.ppm")

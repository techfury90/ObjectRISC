#!/usr/bin/env python3
"""snapshot_desktop.py — render the WM desktop with the OPEN LOOK gray-on-blue
palette, driving the REAL simorisc firmware ops + the REAL palette LUT, and
pixel-check the key colors / geometry against the spec.

Mirrors oriscwm.c's paint sequence (paint_window_chrome -> paint_window_face ->
paint_title_bar [-> paint_menu_button] -> paint_window_border) and its layout
#defines, painting into a single screen framebuffer with ObjFillRect (#0x10D)
+ ObjBlitGlyphs (#0x10C).  The colors come from simorisc's VEC_PALETTE_HEX, so
what this renders is exactly what the live WM composites.

Renders the prior FLAT title bar (before) and the OPEN LOOK olwm title bar this
PR introduces (after), so the rework is visible side by side.  The olwm title
bar (after): a 24px bar flush under the 2px black border, a raised window-menu
button (▽) at the left, the olwm focus model (flat BG1 unfocused vs a recessed
BG3/BG2/white stripe focused), a 1px black separator below the bar, and no
"[X]" close box.  Writes /tmp/orisc-desktop-{before,after}.ppm and asserts the
after geometry/colors.

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

# --- oriscwm.c layout #defines (kept in sync by hand) ----------------------
CELL_W, CELL_H = 8, 16
N_COLS = 80
BORDER_LINE_PX = 2
CELL_AREA_W_PX = CELL_W * N_COLS                       # 640
USABLE_W_PX = (N_COLS + 2) * CELL_W                    # 656
TITLE_X_OFF_PX = CELL_W                                # 8
CONTENT_X_OFF_PX = CELL_W                              # 8
CONTENT_Y_OFF_PX = (1 + 1) * CELL_H                    # 32 (border + title cells)
USABLE_H_PX = (2 + 1 + 8) * CELL_H                     # short window: 8 content rows
CELL_AREA_H_PX = 8 * CELL_H

# NEW olwm title-bar geometry (the "after").
TITLE_BAR_PX = 24
TITLE_INSET = 4                                        # button + stripe float inset
TITLE_INNER_PX = TITLE_BAR_PX - 2 * TITLE_INSET        # 16
TITLE_Y_OFF_PX = BORDER_LINE_PX                        # 2 (flush under border)
MENU_BTN_W = TITLE_INNER_PX                            # 16 (floating square)
TITLE_TEXT_Y_OFF_PX = TITLE_Y_OFF_PX + (TITLE_BAR_PX - CELL_H) // 2   # 6
OL_MM_UL, OL_MM_LR, OL_MM_FILL = 45, 46, 47            # olgl ▽ mark layers (UL/LR/fill)
OL_MENU_MARK_W, OL_MENU_MARK_H = 7, 7                  # 7x7 engraved mark
BEVEL_RAISED, BEVEL_FILL = 0x01, 0x02

# PRIOR flat title-bar geometry (the "before" contrast image).
TITLE_BAR_PX_OLD = CELL_H                              # 16
TITLE_Y_OFF_PX_OLD = CELL_H                            # 16 (one cell row below border)
TITLE_CELL_X_OFF = TITLE_CELL_Y_OFF = 1
CLOSE_BOX_CELLS = 3

# Palette indices (must match simorisc VEC_PALETTE_HEX + oriscwm.c).
WORKSPACE = 9     # OPEN LOOK blue #40a0c0
BG1 = 10          # window face / flat title bar   #cccccc
WHITE_I = 11      # olgx highlight / pane          #f5f5f5
BG2 = 12          # olgx mid / recessed stripe face #b8b8b8
BG3 = 13          # olgx shadow                    #666666
OLW = 8           # pure white #ffffff (prior focused bar)
BLACK = 14        # text / border / separator      #000000
NAVY = 0          # window content bg
BODY_FG = 1       # body text


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
    # font descriptor indices into self.descs
    F_LURS, F_TEXT, F_LUTRS, F_OLGL = 2, 3, 4, 5

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.fb = bytearray(w * h)
        luRS, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
        lutRS, _ = face_blob(f"{BDF}/100dpi/lutRS10.bdf", 32, 95, (8, 16), False)
        olgl, _ = face_blob(f"{BDF}/misc/olgl12.bdf", 19, 167, (47, 47), False)
        self.luRS = luRS
        self.lutRS = lutRS
        self.lutRS_bitmaps_off = 16 + ((95 + 3) & ~3)
        self.descs = [None,
                      _desc(self.fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=w, fb_h=h),
                      _desc(bytearray(luRS)),       # F_LURS
                      _desc(bytearray(b"")),        # F_TEXT (rebound per call)
                      _desc(bytearray(lutRS)),      # F_LUTRS
                      _desc(bytearray(olgl))]       # F_OLGL

    def _cpu(self, font_idx=None, text=None):
        if text is not None:
            self.descs[self.F_TEXT] = _desc(bytearray(text))
        c = FakeCPU(self.descs)
        c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
        if font_idx:
            c.set_opr(2, _ref(font_idx, sim.CAP_R))
            c.set_opr(3, _ref(self.F_TEXT, sim.CAP_R))
        return c

    def fill(self, x, y, w, h, color):
        c = self._cpu()
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, ((w & 0xFFFF) << 16) | (h & 0xFFFF))
        c.set_gpr(6, color)
        sim.primitive_ObjFillRect(c)

    def bevel_raised_edges(self, x, y, w, h, hi, sh, e):
        """Prior look: raised olgx bevel edges only (no fill): hi top+left,
        sh bottom+right; shadow last so it wins the shared corners."""
        self.fill(x, y, w, e, hi)              # top
        self.fill(x, y, e, h, hi)              # left
        self.fill(x, y + h - e, w, e, sh)      # bottom
        self.fill(x + w - e, y, e, h, sh)      # right

    def draw_bevel_box(self, x, y, w, h, mode):
        """Mirror oriscwm.c draw_bevel_box: RAISED = White TL / BG1 face /
        BG3 BR; PRESSED = BG3 TL / BG2 face / White BR.  BEVEL_FILL fills
        the face first; the BR (shadow) edges are drawn last so they win
        the shared corners.  1px chisel edges."""
        e = 1
        raised = mode & BEVEL_RAISED
        hi = WHITE_I if raised else BG3        # top-left edge
        lo = BG3 if raised else WHITE_I        # bottom-right edge
        if mode & BEVEL_FILL:
            self.fill(x, y, w, h, BG1 if raised else BG2)
        self.fill(x, y, w, e, hi)              # top
        self.fill(x, y, e, h, hi)              # left
        self.fill(x, y + h - e, w, e, lo)      # bottom (last → wins corners)
        self.fill(x + w - e, y, e, h, lo)      # right

    def adv_luRS(self, cp):
        gi = cp - 32
        return self.luRS[16 + gi] if 0 <= gi < 95 else 12

    def draw_title(self, x, y, text, fg, bg):
        # proportional luRS, transparent over the bar
        c = self._cpu(font_idx=self.F_LURS, text=text)
        c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
        c.set_gpr(5, 0x80000000 | 0x40000000 | (len(text) << 16) | (fg << 8) | bg)
        c.set_gpr(6, 0); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def draw_menu_mark(self, x, y):
        # olgl ▽ menu mark — 3 engraved layers (UL=BG3, LR=white, fill=BG2),
        # each an EXTENDED transparent blit (olgx_draw_menu_mark's 3D recipe).
        for cp, fg in ((OL_MM_UL, BG3), (OL_MM_LR, WHITE_I), (OL_MM_FILL, BG2)):
            c = self._cpu(font_idx=self.F_OLGL, text=bytes([cp]))
            c.set_gpr(4, ((x & 0xFFFF) << 16) | (y & 0xFFFF))
            c.set_gpr(5, 0x80000000 | 0x40000000 | (1 << 16) | (fg << 8) | BG1)
            c.set_gpr(6, 0); c.set_gpr(7, 0)
            sim.primitive_ObjBlitGlyphs(c)

    def draw_cells(self, cell_x, cell_y, text, fg, bg):
        # legacy 8x16 mono via the lutRS bitmap region
        c = self._cpu(font_idx=self.F_LUTRS, text=text)
        c.set_gpr(4, ((cell_x & 0xFFFF) << 16) | (cell_y & 0xFFFF))
        c.set_gpr(5, (len(text) << 16) | (fg << 8) | bg)
        c.set_gpr(6, self.lutRS_bitmaps_off); c.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(c)

    def _face_content_body(self, wx, wy, body_lines):
        """Shared: gray face, navy content area, mono body text."""
        self.fill(wx, wy, USABLE_W_PX, USABLE_H_PX, BG1)
        self.fill(wx + CONTENT_X_OFF_PX, wy + CONTENT_Y_OFF_PX,
                  CELL_AREA_W_PX, CELL_AREA_H_PX, NAVY)
        for i, line in enumerate(body_lines):
            self.draw_cells((wx + CONTENT_X_OFF_PX) // CELL_W,
                            (wy + CONTENT_Y_OFF_PX) // CELL_H + i,
                            line.encode(), BODY_FG, NAVY)

    def _flat_border(self, wx, wy):
        """Flat 2px black olwm window frame (both before and after)."""
        self.fill(wx, wy, USABLE_W_PX, BORDER_LINE_PX, BLACK)
        self.fill(wx, wy + USABLE_H_PX - BORDER_LINE_PX, USABLE_W_PX, BORDER_LINE_PX, BLACK)
        self.fill(wx, wy, BORDER_LINE_PX, USABLE_H_PX, BLACK)
        self.fill(wx + USABLE_W_PX - BORDER_LINE_PX, wy, BORDER_LINE_PX, USABLE_H_PX, BLACK)

    def window_flat(self, wx, wy, title, focused, body_lines):
        """PRIOR look (before): flat 16px title bar floating one cell below
        the border, white-focused / BG1-unfocused, with a "[X]" close box."""
        self._face_content_body(wx, wy, body_lines)
        bar_bg = OLW if focused else BG1
        self.fill(wx + TITLE_X_OFF_PX, wy + TITLE_Y_OFF_PX_OLD,
                  CELL_AREA_W_PX, TITLE_BAR_PX_OLD, bar_bg)
        avail = (N_COLS - CLOSE_BOX_CELLS) * CELL_W
        tb = title.encode()
        tpx = 0; n = 0
        while n < len(tb) and tpx + self.adv_luRS(tb[n]) <= avail:
            tpx += self.adv_luRS(tb[n]); n += 1
        sx = wx + TITLE_X_OFF_PX + (avail - tpx) // 2
        self.draw_title(sx, wy + TITLE_Y_OFF_PX_OLD, tb[:n], BLACK, bar_bg)
        box_col = (wx // CELL_W) + TITLE_CELL_X_OFF + (N_COLS - CLOSE_BOX_CELLS)
        self.draw_cells(box_col, (wy // CELL_H) + TITLE_CELL_Y_OFF,
                        b"[X]", BLACK, bar_bg)
        self._flat_border(wx, wy)

    def window_olwm(self, wx, wy, title, focused, body_lines):
        """NEW olwm look (after): 24px bar flush under the border, raised
        window-menu button (▽) at the left, recessed focus stripe (focused)
        vs flat BG1 (unfocused), 1px black separator below, no "[X]"."""
        self._face_content_body(wx, wy, body_lines)
        # Bar base: flat BG1 for both focus states.
        self.fill(wx + TITLE_X_OFF_PX, wy + TITLE_Y_OFF_PX,
                  CELL_AREA_W_PX, TITLE_BAR_PX, BG1)
        span_x = wx + TITLE_X_OFF_PX + MENU_BTN_W + TITLE_INSET
        span_w = CELL_AREA_W_PX - MENU_BTN_W - 2 * TITLE_INSET
        inner_y = wy + TITLE_Y_OFF_PX + TITLE_INSET
        # Focused: recessed stripe (PRESSED bevel) FLOATING in the title-text area.
        if focused:
            self.draw_bevel_box(span_x, inner_y, span_w, TITLE_INNER_PX, BEVEL_FILL)
        # Raised window-menu button (floating) + ▽ engraved mark, on top, at left.
        self.draw_bevel_box(wx + TITLE_X_OFF_PX, inner_y,
                            MENU_BTN_W, TITLE_INNER_PX, BEVEL_RAISED | BEVEL_FILL)
        gx = wx + TITLE_X_OFF_PX + (MENU_BTN_W - OL_MENU_MARK_W) // 2
        gy = inner_y + (TITLE_INNER_PX - OL_MENU_MARK_H) // 2
        self.draw_menu_mark(gx, gy)
        # Title text centred in the span right of the button, v-centred.
        tb = title.encode()
        tpx = 0; n = 0
        while n < len(tb) and tpx + self.adv_luRS(tb[n]) <= span_w:
            tpx += self.adv_luRS(tb[n]); n += 1
        sx = span_x + (span_w - tpx) // 2
        text_bg = BG2 if focused else BG1
        self.draw_title(sx, wy + TITLE_TEXT_Y_OFF_PX, tb[:n], BLACK, text_bg)
        # 1px black separator directly below the bar.
        self.fill(wx + TITLE_X_OFF_PX, wy + TITLE_Y_OFF_PX + TITLE_BAR_PX,
                  CELL_AREA_W_PX, 1, BLACK)
        self._flat_border(wx, wy)

    def save(self, path):
        lut = sim._build_palette_lut()
        rgb = b"".join(lut[bb] for bb in self.fb)
        with open(path, "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (self.w, self.h))
            fh.write(rgb)

    def px(self, x, y):
        lut = sim._build_palette_lut()
        return tuple(lut[self.fb[y * self.w + x]])


def render(olwm, path):
    W, H = 760, 540
    s = Scene(W, H)
    s.fill(0, 0, W, H, WORKSPACE)
    paint = s.window_olwm if olwm else s.window_flat
    paint(40, 40, "Object RISC Shell", True,
          ["$ ls", "hello.orx  shell.orx", "$ "])
    paint(96, 300, "ps", False, ["PID  CMD", "  1  shell"])
    s.save(path)
    return s


if __name__ == "__main__":
    render(False, "/tmp/orisc-desktop-before.ppm")   # prior flat 16px bar + [X]
    s = render(True, "/tmp/orisc-desktop-after.ppm")  # OPEN LOOK olwm title bars

    # --- Pixel-check the AFTER (olwm) geometry + focus model ---------------
    # Focused window wx=40 wy=40: border y40-41, bar (BG1 base) y42-65, the
    # button+stripe FLOAT inset to y46-61 (TITLE_INSET=4), separator y66, BG1
    # pad y67-71, content y72+.  Button screen x48-63; title stripe x68-683.
    BG1c, BG2c, BG3c = (204, 204, 204), (184, 184, 184), (102, 102, 102)
    WHITEc, BLACKc, BLUEc, NAVYc = (245, 245, 245), (0, 0, 0), \
        (64, 160, 192), (10, 10, 20)
    checks = [
        ("workspace",                     s.px(10, 10),  BLUEc),
        ("window face (left ring pad)",   s.px(44, 100), BG1c),
        # flat 2px black border, title bar flush under it
        ("border top px0",                s.px(300, 40), BLACKc),
        ("border top px1",                s.px(300, 41), BLACKc),
        ("border left px0",               s.px(40, 120), BLACKc),
        ("border left px1",               s.px(41, 120), BLACKc),
        # the stripe FLOATS: BG1 bar base shows above (y44) and below (y63) it
        ("bar base above float (BG1)",    s.px(200, 44), BG1c),
        ("bar base below float (BG1)",    s.px(200, 63), BG1c),
        # focused recessed stripe (1px edges): BG3 top / BG2 face / white bottom
        ("focused stripe top edge (BG3)", s.px(200, 46), BG3c),
        ("focused stripe face (BG2)",     s.px(200, 52), BG2c),
        ("focused stripe bottom (white)", s.px(200, 61), WHITEc),
        # raised floating menu button (White TL / BG1 face / BG3 BR), 1px edges
        ("menu btn top edge (white)",     s.px(54, 46),  WHITEc),
        ("menu btn left edge (white)",    s.px(48, 53),  WHITEc),
        ("menu btn face (BG1)",           s.px(50, 48),  BG1c),
        ("menu btn shadow bottom (BG3)",  s.px(54, 61),  BG3c),
        ("menu btn shadow right (BG3)",   s.px(63, 54),  BG3c),
        # 1px black separator below the focused bar
        ("separator (black)",             s.px(300, 66), BLACKc),
        # content still at y-offset 32: BG1 pad above, navy content at y72
        ("BG1 frame pad above content",   s.px(300, 70), BG1c),
        ("content navy at y32 offset",    s.px(300, 72), NAVYc),
        # unfocused window wx=96 wy=300: flat BG1 bar (NO stripe), floating menu
        # button, separator.  bar y302-325, float y306-321, separator y326.
        ("unfocused flat bar (BG1)",      s.px(300, 310), BG1c),
        ("unfocused no stripe (BG1)",     s.px(680, 312), BG1c),
        ("unfocused menu btn (white)",    s.px(110, 306), WHITEc),
        ("unfocused separator (black)",   s.px(300, 326), BLACKc),
    ]
    for name, got, want in checks:
        assert got == want, f"{name}: {got} != {want}"
    print("OPEN LOOK olwm title bars OK:")
    for n, g, _ in checks:
        print(f"  {n} = {g}")
    print("wrote /tmp/orisc-desktop-before.ppm (prior flat bar + [X]) and "
          "/tmp/orisc-desktop-after.ppm (olwm menu button + focus stripe)")

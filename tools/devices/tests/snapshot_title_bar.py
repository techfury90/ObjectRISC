#!/usr/bin/env python3
"""snapshot_title_bar.py — render a WM title bar exactly as paint_title_bar
composes it, driving the REAL simorisc firmware ops, and write a PPM.

This is the headless companion to the WM's Cmd+S framebuffer snapshot: it
reproduces oriscwm.c's title-bar layout (its own #define constants, mirrored
below) and paints it with the same three firmware primitives the WM uses —

  * ObjFillRect    (#0x10D)  the bright-white focused bar
  * ObjBlitGlyphs  (#0x10C, EXTENDED)  the proportional Lucida Sans title
  * ObjBlitGlyphs  (#0x10C, LEGACY)    the "[X]" close box (8x16 mono)

so the pixels are produced by the shipping firmware, not a re-implementation.
The close box is rendered through the legacy path against the lutRS face's
own 8x16 bitmap region — the same shape font_8x16 has — so both paths are
exercised against real baked data.

Usage:  python3 tools/devices/tests/snapshot_title_bar.py ["Window Title"]
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
BORDER_CELLS_X = BORDER_CELLS_Y = 1
TITLE_BAR_PX = CELL_H                       # 16
CELL_AREA_W_PX = CELL_W * N_COLS            # 640
USABLE_W_PX = (N_COLS + 2 * BORDER_CELLS_X) * CELL_W   # 656
TITLE_X_OFF_PX = BORDER_CELLS_X * CELL_W    # 8
TITLE_Y_OFF_PX = BORDER_CELLS_Y * CELL_H    # 16
TITLE_CELL_X_OFF = BORDER_CELLS_X           # 1
TITLE_CELL_Y_OFF = BORDER_CELLS_Y           # 1
CLOSE_BOX_CELLS = 3
WM_BG_COLOR = 0                             # navy
WM_BORDER_COLOR = 1
WM_TITLE_FOCUSED_BG = 8                     # bright white
WM_TITLE_TEXT_FG = WM_BG_COLOR             # navy text


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
    title = (sys.argv[1] if len(sys.argv) > 1 else "Object RISC Shell").encode()

    luRS, luRS_i = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    lutRS, _ = face_blob(f"{BDF}/100dpi/lutRS10.bdf", 32, 95, (8, 16), False)
    # The legacy path reads 16-byte glyphs from font_off; lutRS's bitmap
    # region (after magic+header+padded widths) is exactly that layout.
    lutRS_bitmaps_off = 16 + ((95 + 3) & ~3)

    fb_w, fb_h = USABLE_W_PX, TITLE_Y_OFF_PX + TITLE_BAR_PX + 4   # border + bar
    fb = bytearray([WM_BG_COLOR] * fb_w * fb_h)
    # font + text descriptors live alongside the FB
    text_buf = bytearray(title) + b"[X]"
    descs = [None,
             _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=fb_w, fb_h=fb_h),
             _desc(bytearray(luRS)),          # idx 2: luRS face (extended)
             _desc(text_buf),                 # idx 3: title + "[X]"
             _desc(bytearray(lutRS))]         # idx 4: lutRS face (legacy)

    def cpu_for(font_idx):
        c = FakeCPU(descs)
        c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
        c.set_opr(2, _ref(font_idx, sim.CAP_R))
        c.set_opr(3, _ref(3, sim.CAP_R))
        return c

    # 1) Fill the focused title bar bright white (ObjFillRect).
    c = FakeCPU(descs); c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
    c.set_gpr(4, (TITLE_X_OFF_PX << 16) | TITLE_Y_OFF_PX)
    c.set_gpr(5, (CELL_AREA_W_PX << 16) | TITLE_BAR_PX)
    c.set_gpr(6, WM_TITLE_FOCUSED_BG)
    sim.primitive_ObjFillRect(c)

    # 2) Centre + draw the proportional title (extended ObjBlitGlyphs).
    avail_px = (N_COLS - CLOSE_BOX_CELLS) * CELL_W
    # measure via the same width table the firmware uses
    def adv(cp):
        gi = cp - 32
        if gi < 0 or gi >= 95:
            return luRS_i["cell_w"]
        return luRS[16 + gi]
    title_px = 0; n = 0
    while n < len(title) and title_px + adv(title[n]) <= avail_px:
        title_px += adv(title[n]); n += 1
    start_x = TITLE_X_OFF_PX + (avail_px - title_px) // 2
    c = cpu_for(2)
    c.set_gpr(4, (start_x << 16) | TITLE_Y_OFF_PX)
    c.set_gpr(5, 0x80000000 | 0x40000000 | (n << 16)
              | (WM_TITLE_TEXT_FG << 8) | WM_TITLE_FOCUSED_BG)   # ext+transparent
    c.set_gpr(6, 0); c.set_gpr(7, 0)
    sim.primitive_ObjBlitGlyphs(c)

    # 3) Close box "[X]" via the LEGACY cell path (lutRS bitmap region).
    box_col = TITLE_CELL_X_OFF + (N_COLS - CLOSE_BOX_CELLS)
    c = cpu_for(4)
    c.set_gpr(4, (box_col << 16) | TITLE_CELL_Y_OFF)
    c.set_gpr(5, (CLOSE_BOX_CELLS << 16) | (WM_TITLE_TEXT_FG << 8) | WM_TITLE_FOCUSED_BG)
    c.set_gpr(6, lutRS_bitmaps_off)
    c.set_gpr(7, len(title))          # "[X]" starts after the title in text_buf
    sim.primitive_ObjBlitGlyphs(c)

    # 4) A 2px top border line (ObjFillRect), like the WM chrome.
    c = FakeCPU(descs); c.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
    c.set_gpr(4, 0); c.set_gpr(5, (fb_w << 16) | 2); c.set_gpr(6, WM_BORDER_COLOR)
    sim.primitive_ObjFillRect(c)

    lut = sim._build_palette_lut()
    rgb = b"".join(lut[b] for b in fb)
    out = "/tmp/orisc-title-bar.ppm"
    with open(out, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (fb_w, fb_h))
        fh.write(rgb)
    print(f"wrote {out}  ({fb_w}x{fb_h}); title '{title.decode()}' "
          f"{n} glyphs, {title_px}px, start_x={start_x}")


if __name__ == "__main__":
    main()

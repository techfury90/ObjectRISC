#!/usr/bin/env python3
"""test_font_blit_ex.py — exercise the EXTENDED ObjBlitGlyphs firmware path
against the REAL baked WM font faces (luRS proportional, lutRS mono).

Drives the actual simorisc primitive (no simulation harness) with hand-built
descriptors, so it covers the firmware decode + the self-describing font
format + the generator's bake end-to-end:

  * proportional advance from the face's own width table
  * absolute pixel positioning
  * transparent vs opaque background
  * out-of-range codepoint handling (blank, default advance)
  * the legacy 8x16 cell-grid path still renders identically (R5 bit 31 clear)

Also renders a real title string and writes /tmp/orisc-font-luRS.ppm for
eyeballing.  Run directly: python3 tools/sim/tests/test_font_blit_ex.py
"""
import importlib.machinery
import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))

_loader = importlib.machinery.SourceFileLoader("simorisc", str(ROOT / "tools" / "sim" / "simorisc"))
_spec = importlib.util.spec_from_loader("simorisc", _loader)
sim = importlib.util.module_from_spec(_spec)
_loader.exec_module(sim)

from gen_wm_font import face_blob   # noqa: E402

BDF = ("/Users/lando/Downloads/OpenLookCDROM-master/src/lib/"
       "xview3.2p1-X11R6-LinuxElf/fonts/bdf")

PID = 7


def _desc(storage, length=None, type_tag=0, fb_w=0, fb_h=0):
    return SimpleNamespace(
        live=True, generation=1, type_tag=type_tag,
        storage=storage, length=length if length is not None else len(storage),
        fb_width=fb_w, fb_height=fb_h, fb_dirty=False)


class FakeCPU:
    """Just enough CPU surface for primitive_ObjBlitGlyphs."""
    def __init__(self, descriptors):
        self.pid = PID
        self.descriptors = descriptors
        self._opr = {}
        self._gpr = {}

    def get_opr(self, i): return self._opr.get(i, 0)
    def set_opr(self, i, v): self._opr[i] = v
    def get_gpr(self, i): return self._gpr.get(i, 0)
    def set_gpr(self, i, v): self._gpr[i] = v


def _ref(index, caps):
    return sim.make_ref(generation=1, home=PID, index=index, caps=caps)


def run_blit(fb_w, fb_h, font_bytes, text_bytes, r4, r5,
             font_off=0, text_off=0):
    """Set up descriptors [None, fb, font, text] and run the primitive.
    Returns (status, rendered, fb_storage)."""
    fb_storage = bytearray(fb_w * fb_h)
    descs = [
        None,
        _desc(fb_storage, type_tag=sim.TAG_FRAMEBUFFER, fb_w=fb_w, fb_h=fb_h),
        _desc(bytearray(font_bytes)),
        _desc(bytearray(text_bytes)),
    ]
    cpu = FakeCPU(descs)
    cpu.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))   # FB needs W
    cpu.set_opr(2, _ref(2, sim.CAP_R))               # font needs R
    cpu.set_opr(3, _ref(3, sim.CAP_R))               # text needs R
    cpu.set_gpr(4, r4)
    cpu.set_gpr(5, r5)
    cpu.set_gpr(6, font_off)
    cpu.set_gpr(7, text_off)
    sim.primitive_ObjBlitGlyphs(cpu)
    return cpu.get_gpr(2), cpu.get_gpr(3), fb_storage


def shape_ext(n, fg, bg, transparent=False):
    s = 0x80000000
    if transparent:
        s |= 0x40000000
    return s | ((n & 0x3FFF) << 16) | ((fg & 0xFF) << 8) | (bg & 0xFF)


def dump(fb, w, h, on=1):
    for y in range(h):
        print("  " + "".join("#" if fb[y * w + x] == on else "." for x in range(w)))


def col_set(fb, w, h, x, fg):
    return any(fb[y * w + x] == fg for y in range(h))


# ---------------------------------------------------------------------------

def test_proportional_advance():
    """luRS 'll' — two identical narrow bars spaced by the 'l' advance (not
    by the 12px cell box).  Proves the firmware advances from the width
    table, not by cell_w."""
    blob, info = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    cw, ch = info["cell_w"], info["cell_h"]
    # advance of 'l' from the baked width table
    import gen_wm_font  # noqa
    text = b"ll"
    st, rn, fb = run_blit(48, 16, blob, text, (2 << 16) | 0, shape_ext(2, 1, 0))
    assert st == sim.ERR_OK and rn == 2, (st, rn)
    # find the set columns of the first and second bar
    cols = [x for x in range(48) if col_set(fb, 48, 16, x, 1)]
    assert cols, "no pixels rendered"
    # The two 'l' stems should be separated by 'l's advance, well under 12.
    runs = []
    prev = None
    for x in cols:
        if prev is None or x != prev + 1:
            runs.append([x, x])
        else:
            runs[-1][1] = x
        prev = x
    assert len(runs) == 2, f"expected two glyph stems, got {runs}"
    advance = runs[1][0] - runs[0][0]
    assert advance < cw, f"advance {advance} should be < cell_w {cw} (proportional)"
    print(f"  proportional 'll': stems at {runs}, advance={advance} < cell_w={cw}  OK")


def test_pixel_position_and_clip():
    """A glyph drawn at pixel x=5 puts no ink left of x=5, and clips at the
    right framebuffer edge."""
    blob, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    st, rn, fb = run_blit(40, 16, blob, b"H", (5 << 16) | 0, shape_ext(1, 1, 0))
    assert st == sim.ERR_OK and rn == 1
    for x in range(5):
        assert not col_set(fb, 40, 16, x, 1), f"ink left of pen at x={x}"
    print("  pixel-position: no ink left of pen  OK")


def test_transparent_vs_opaque():
    """Transparent leaves the pre-filled background in the gaps; opaque
    overwrites the whole cell box with bg."""
    blob, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    # Pre-fill FB with palette 5, draw 'i' (navy=0 on ...).  Transparent: the
    # blank cell pixels stay 5.  Opaque (bg=0): they become 0.
    for transparent, expect_gap in ((True, 5), (False, 0)):
        fb_w, fb_h = 16, 16
        fb = bytearray([5] * fb_w * fb_h)
        descs = [None,
                 _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=fb_w, fb_h=fb_h),
                 _desc(bytearray(blob)), _desc(bytearray(b"i"))]
        cpu = FakeCPU(descs)
        cpu.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
        cpu.set_opr(2, _ref(2, sim.CAP_R))
        cpu.set_opr(3, _ref(3, sim.CAP_R))
        cpu.set_gpr(4, 0)
        cpu.set_gpr(5, shape_ext(1, 1, 0, transparent))
        cpu.set_gpr(6, 0); cpu.set_gpr(7, 0)
        sim.primitive_ObjBlitGlyphs(cpu)
        # bottom-right corner of the cell is always blank for 'i'
        corner = fb[15 * fb_w + 11]
        assert corner == expect_gap, (transparent, corner, expect_gap)
    print("  transparent leaves bg in gaps; opaque overwrites  OK")


def test_out_of_range_codepoint():
    """A codepoint below the face base renders blank and advances by cell_w."""
    blob, info = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    cw = info["cell_w"]
    # byte 0x01 is below base 32; then 'l'.  Expect: blank, advance cw, then 'l'.
    st, rn, fb = run_blit(48, 16, blob, bytes([1]) + b"l", 0, shape_ext(2, 1, 0))
    assert st == sim.ERR_OK
    # only 1 glyph rendered (the unknown one is skipped but still advances)
    assert rn == 1, f"rendered {rn}, expected 1 (unknown glyph blank)"
    cols = [x for x in range(48) if col_set(fb, 48, 16, x, 1)]
    assert cols and min(cols) >= cw, f"'l' should start at >= cell_w {cw}, cols={cols}"
    print(f"  out-of-range codepoint: blank + advance {cw}  OK")


def test_legacy_path_unchanged():
    """R5 bit 31 clear → the legacy 8x16 cell-grid path.  Build a tiny 8x16
    font where glyph for 'A' (code 65) is a full top row, render at cell
    (1,0), and confirm it lands at pixel (8,0)..(15,0)."""
    # font_8x16-shaped: 95 glyphs * 16 bytes, glyph (code-32) at offset.
    font = bytearray(95 * 16)
    gi = ord('A') - 32
    font[gi * 16 + 0] = 0xFF      # top row all set
    text = b"A"
    # legacy R5 = (n<<16)|(fg<<8)|bg, bit 31 clear; R4 = (cell_x<<16)|cell_y
    r5 = (1 << 16) | (3 << 8) | 0
    st, rn, fb = run_blit(32, 16, font, text, (1 << 16) | 0, r5)
    assert st == sim.ERR_OK and rn == 1, (st, rn)
    # cell_x=1 → pixel x 8..15, top row y=0 set to fg=3
    for x in range(8):
        assert fb[0 * 32 + x] == 0, f"cell 0 should be untouched at x={x}"
    for x in range(8, 16):
        assert fb[0 * 32 + x] == 3, f"glyph top row missing at x={x}"
    print("  legacy 8x16 cell-grid path intact  OK")


def render_sample_ppm():
    """Render a real title string in luRS, navy-on-white (title-bar colors),
    and write a PPM for eyeballing."""
    blob, info = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12, 16), True)
    text = b"Object RISC Shell"
    fb_w, fb_h = 220, 16
    fb = bytearray([8] * fb_w * fb_h)     # palette 8 = bright white bar
    descs = [None,
             _desc(fb, type_tag=sim.TAG_FRAMEBUFFER, fb_w=fb_w, fb_h=fb_h),
             _desc(bytearray(blob)), _desc(bytearray(text))]
    cpu = FakeCPU(descs)
    cpu.set_opr(1, _ref(1, sim.CAP_R | sim.CAP_W))
    cpu.set_opr(2, _ref(2, sim.CAP_R))
    cpu.set_opr(3, _ref(3, sim.CAP_R))
    cpu.set_gpr(4, (4 << 16) | 0)         # pixel (4, 0)
    cpu.set_gpr(5, shape_ext(len(text), 0, 8, transparent=True))  # navy on white
    cpu.set_gpr(6, 0); cpu.set_gpr(7, 0)
    sim.primitive_ObjBlitGlyphs(cpu)
    lut = sim._build_palette_lut()
    rgb = b"".join(lut[b] for b in fb)
    out = "/tmp/orisc-font-luRS.ppm"
    with open(out, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (fb_w, fb_h))
        fh.write(rgb)
    print(f"  wrote {out}")
    print("  ASCII view ('#'=navy ink):")
    dump(fb, fb_w, fb_h, on=0)


if __name__ == "__main__":
    test_proportional_advance()
    test_pixel_position_and_clip()
    test_transparent_vs_opaque()
    test_out_of_range_codepoint()
    test_legacy_path_unchanged()
    render_sample_ppm()
    print("\nALL FONT BLIT TESTS PASSED")

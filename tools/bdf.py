#!/usr/bin/env python3
"""bdf.py — minimal X11 BDF bitmap-font reader for the WM font manager.

Parses an Adobe BDF font (plain text) and bakes each glyph in a chosen
codepoint range into a fixed CELL_W x CELL_H 1-bit cell, baseline-aligned
and (for a fixed cell) vertically centered using the font's FONT_ASCENT /
FONT_DESCENT.  Returns, per codepoint, the cell bitmap rows plus the
glyph's advance width (DWIDTH) — exactly the two things a proportional
renderer needs.

Used by tools/gen_wm_font.py --face to bake the OPEN LOOK faces (Lucida
Sans `luRS`, Lucida Sans Typewriter `lutRS`, the OPEN LOOK glyph font
`olgl`) straight out of the OpenLook CD-ROM BDF sources into a
self-describing WM font object.  Self-contained (no Pillow / freetype).

BDF reference: each glyph carries BBX (ink width, height, x-offset,
y-offset-of-bottom-from-baseline) and DWIDTH (pen advance); BITMAP rows
are hex, each padded to ceil(width/8) bytes, most-significant-bit-first,
leftmost pixel = MSB of the first byte.

Usage as a tool (renders sample glyphs for eyeballing):
    python3 tools/bdf.py /path/to/font.bdf [LO HI]
"""
import sys


def _parse(path):
    """Return (header, glyphs).

    header: dict with ascent, descent, fbbx (w,h,xoff,yoff).
    glyphs: {codepoint: dict(dwidth, bbx=(w,h,xoff,yoff), rows=[int,...])}
            where each rows[] entry is the hex bytes of one scanline packed
            into a single int (first hex byte = most-significant byte).
    """
    ascent = descent = None
    fbbx = (0, 0, 0, 0)
    glyphs = {}
    cur = None          # codepoint of the glyph being parsed
    in_bitmap = False
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if not parts:
                continue
            kw = parts[0]
            if in_bitmap:
                if kw == "ENDCHAR":
                    in_bitmap = False
                    cur = None
                else:
                    # One scanline of hex (already byte-padded by the font).
                    nbytes = len(parts[0]) // 2
                    glyphs[cur]["rows"].append((int(parts[0], 16), nbytes))
                continue
            if kw == "FONT_ASCENT":
                ascent = int(parts[1])
            elif kw == "FONT_DESCENT":
                descent = int(parts[1])
            elif kw == "FONTBOUNDINGBOX":
                fbbx = tuple(int(x) for x in parts[1:5])
            elif kw == "ENCODING":
                cur = int(parts[1])
                if cur >= 0:
                    glyphs[cur] = {"dwidth": 0, "bbx": (0, 0, 0, 0),
                                   "rows": []}
            elif kw == "DWIDTH" and cur is not None and cur >= 0:
                glyphs[cur]["dwidth"] = int(parts[1])
            elif kw == "BBX" and cur is not None and cur >= 0:
                glyphs[cur]["bbx"] = tuple(int(x) for x in parts[1:5])
            elif kw == "BITMAP" and cur is not None and cur >= 0:
                in_bitmap = True
    if ascent is None:
        ascent = fbbx[1] + fbbx[3]      # height + yoff
    if descent is None:
        descent = -fbbx[3]
    return dict(ascent=ascent, descent=descent, fbbx=fbbx), glyphs


def _src_bit(rows, sx, sy):
    """True if pixel (sx, sy) of a glyph's source bitmap is set."""
    if sy < 0 or sy >= len(rows):
        return False
    val, nbytes = rows[sy]
    width_bits = nbytes * 8
    if sx < 0 or sx >= width_bits:
        return False
    return (val >> (width_bits - 1 - sx)) & 1


def bake(path, base, count, cell_w=None, cell_h=None):
    """Bake codepoints [base, base+count) into fixed cells.

    Returns (cells, widths, info):
      cells:  list of `count` glyph bitmaps; each is a list of cell_h ints,
              one per row, where bit (1 << (cell_w-1-x)) set means pixel x
              is on (MSB = leftmost pixel within the cell_w-wide field).
      widths: list of `count` advance widths in px (clamped to 0..255).
      info:   dict(cell_w, cell_h, baseline, clipped, present).

    When cell_w / cell_h are None they are auto-fit to the union of the
    selected glyphs' ink boxes (used for the OPEN LOOK glyph font, whose
    nominal bounding box is far larger than any real glyph).  When given
    explicitly the glyphs are baseline-centered in the cell using the
    font's FONT_ASCENT / FONT_DESCENT, and anything spilling outside the
    cell is clipped (and reported)."""
    hdr, glyphs = _parse(path)
    sel = [cp for cp in range(base, base + count) if cp in glyphs]

    if cell_w is None or cell_h is None:
        # Auto-fit: cell spans the union of selected glyphs' ink boxes.
        x_lo = min((glyphs[cp]["bbx"][2] for cp in sel), default=0)
        x_hi = max((glyphs[cp]["bbx"][2] + glyphs[cp]["bbx"][0]
                    for cp in sel), default=cell_w or 1)
        top = max((glyphs[cp]["bbx"][3] + glyphs[cp]["bbx"][1]
                   for cp in sel), default=hdr["ascent"])      # rows above baseline
        bot = min((glyphs[cp]["bbx"][3] for cp in sel), default=-hdr["descent"])
        cell_w = max(1, x_hi - x_lo)
        cell_h = max(1, top - bot)
        x_shift = -x_lo
        baseline = top
    else:
        # Fixed cell: center the font's ascent+descent box vertically.
        font_h = hdr["ascent"] + hdr["descent"]
        top_pad = (cell_h - font_h) // 2
        baseline = top_pad + hdr["ascent"]
        x_shift = 0

    cells, widths, clipped = [], [], []
    for cp in range(base, base + count):
        rows = [0] * cell_h
        g = glyphs.get(cp)
        if g is None:
            cells.append(rows)
            widths.append(min(255, max(0, cell_w)))     # blank: default advance
            continue
        gw, gh, gxoff, gyoff = g["bbx"]
        # The glyph's top scanline sits (gyoff + gh) rows above the baseline.
        top_row = baseline - (gyoff + gh)
        for sy in range(gh):
            cy = top_row + sy
            for sx in range(gw):
                if not _src_bit(g["rows"], sx, sy):
                    continue
                cx = x_shift + gxoff + sx
                if 0 <= cx < cell_w and 0 <= cy < cell_h:
                    rows[cy] |= 1 << (cell_w - 1 - cx)
                elif cp not in clipped:
                    clipped.append(cp)
        cells.append(rows)
        widths.append(min(255, max(0, g["dwidth"])))

    info = dict(cell_w=cell_w, cell_h=cell_h, baseline=baseline,
                clipped=clipped, present=len(sel),
                ascent=hdr["ascent"], descent=hdr["descent"])
    return cells, widths, info


if __name__ == "__main__":
    path = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 127
    cw = int(sys.argv[4]) if len(sys.argv) > 4 else None
    ch = int(sys.argv[5]) if len(sys.argv) > 5 else None
    cells, widths, info = bake(path, lo, hi - lo, cw, ch)
    print("INFO:", info)
    for cp in range(lo, hi):
        gi = cp - lo
        ch_repr = chr(cp) if 32 < cp < 127 else f"\\x{cp:02x}"
        print(f"\n{ch_repr!r}  cp={cp}  advance={widths[gi]}")
        for r in cells[gi]:
            print("  " + "".join('#' if r & (1 << (info['cell_w'] - 1 - x))
                                 else '.' for x in range(info['cell_w'])))

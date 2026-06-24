#!/usr/bin/env python3
"""pcf.py — minimal X11 PCF bitmap-font reader.

Parses the encoding, metrics, and bitmap tables of an X11 PCF font and packs
each codepoint into a CELL_W x CELL_H 1-bit cell, baseline-aligned and centered
vertically.  Self-contained (no Pillow — its PcfFontFile silently drops
codepoints 95..126 on the OpenWindows lutRS fonts, i.e. all lowercase).

Used by gen_wm_font.py --pcf to bake the OpenWindows Lucida Sans Typewriter
bitmaps (Solaris 2.6 SUNWxwoft, lutRS10.pcf @ 100dpi) into the WM's 8x16 font,
replacing the ImageMagick rasterization of the TTF.  Also the groundwork for a
future runtime bitmap-font manager.

PCF format reference: the standard X11 PCF layout (header + table-of-contents +
per-table format word selecting byte/bit order and glyph padding).

Usage as a tool (renders sample glyphs for eyeballing):
    python3 tools/pcf.py /path/to/font.pcf
"""
import struct
import sys

PCF_METRICS            = 1 << 2
PCF_BITMAPS            = 1 << 3
PCF_BDF_ENCODINGS      = 1 << 5
PCF_COMPRESSED_METRICS = 0x100
PCF_BYTE_MASK          = 1 << 2   # set => big-endian within the table
PCF_BIT_MASK           = 1 << 3   # set => most-significant-bit-first scanlines
PCF_GLYPH_PAD_MASK     = 3


def _e(fmt):
    return '>' if (fmt & PCF_BYTE_MASK) else '<'


def _load_tables(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x01fcp':
        sys.exit("not a PCF font: " + path)
    (n,) = struct.unpack_from('<i', d, 4)
    off, tables = 8, {}
    for _ in range(n):
        typ, fmt, _size, offset = struct.unpack_from('<iiii', d, off)
        off += 16
        tables[typ] = (fmt, offset)
    return d, tables


def _metrics(d, fmt, off):
    e, p, out = _e(fmt), off + 4, []
    if fmt & PCF_COMPRESSED_METRICS:
        (c,) = struct.unpack_from(e + 'h', d, p); p += 2
        for _ in range(c):
            b = d[p:p + 5]; p += 5
            out.append((b[0] - 0x80, b[1] - 0x80, b[2] - 0x80,
                        b[3] - 0x80, b[4] - 0x80))
    else:
        (c,) = struct.unpack_from(e + 'i', d, p); p += 4
        for _ in range(c):
            lsb, rsb, w, a, de, _attr = struct.unpack_from(e + 'hhhhhH', d, p)
            p += 12
            out.append((lsb, rsb, w, a, de))
    return out   # each: (lsb, rsb, width, ascent, descent)


def _encoding(d, fmt, off):
    e, p = _e(fmt), off + 4
    minc2, maxc2, minb1, maxb1, _def = struct.unpack_from(e + 'hhhhh', d, p)
    p += 10
    cols, rows = maxc2 - minc2 + 1, maxb1 - minb1 + 1
    idx = struct.unpack_from(e + ('H' * (cols * rows)), d, p)
    enc, k = {}, 0
    for b1 in range(minb1, maxb1 + 1):
        for b2 in range(minc2, maxc2 + 1):
            gi = idx[k]; k += 1
            if gi != 0xFFFF:
                cp = (b1 << 8 | b2) if (minb1 or maxb1) else b2
                enc[cp] = gi
    return enc


def _bitmaps(d, fmt, off):
    e, p = _e(fmt), off + 4
    (gc,) = struct.unpack_from(e + 'i', d, p); p += 4
    offs = struct.unpack_from(e + ('i' * gc), d, p); p += 4 * gc
    sizes = struct.unpack_from(e + 'iiii', d, p); p += 16
    pad = 1 << (fmt & PCF_GLYPH_PAD_MASK)
    data = d[p:p + sizes[fmt & PCF_GLYPH_PAD_MASK]]
    return offs, data, pad, bool(fmt & PCF_BIT_MASK)


def pcf_cells(path, CELL_W=8, CELL_H=16, lo=32, hi=127):
    """Return (cells, info).

    cells:  {codepoint: [CELL_H ints]} — one int per row, bit (1<<(7-x)) set
            means pixel x is on (MSB = leftmost), for codepoints lo..hi-1.
    info:   dict with ascent/descent/cell_h/baseline/present/total/fits/clipped.
    Glyphs are baseline-aligned and the whole font is centered in the cell.
    """
    d, t = _load_tables(path)
    M = _metrics(d, *t[PCF_METRICS])
    E = _encoding(d, *t[PCF_BDF_ENCODINGS])
    OFFS, BM, pad, msb = _bitmaps(d, *t[PCF_BITMAPS])

    gis = [E[c] for c in range(lo, hi) if c in E]
    ascent  = max(M[g][3] for g in gis)
    descent = max(M[g][4] for g in gis)
    cell_h  = ascent + descent
    baseline = (CELL_H - cell_h) // 2 + ascent

    cells, clipped = {}, []
    for c in range(lo, hi):
        rows = [0] * CELL_H
        gi = E.get(c)
        if gi is not None:
            lsb, _rsb, w, a, de = M[gi]
            bpr = (((w + 7) // 8 + pad - 1) // pad) * pad
            base = OFFS[gi]
            for sy in range(a + de):
                rb = BM[base + sy * bpr: base + sy * bpr + bpr]
                for sx in range(w):
                    byte = rb[sx // 8] if (sx // 8) < len(rb) else 0
                    bit = ((byte >> (7 - (sx % 8))) & 1 if msb
                           else (byte >> (sx % 8)) & 1)
                    if bit:
                        cx, cy = lsb + sx, baseline - a + sy
                        if 0 <= cx < CELL_W and 0 <= cy < CELL_H:
                            rows[cy] |= 1 << (7 - cx)
                        elif c not in clipped:
                            clipped.append(c)
        cells[c] = rows

    info = dict(ascent=ascent, descent=descent, cell_h=cell_h, baseline=baseline,
                present=len(gis), total=hi - lo, fits=(cell_h <= CELL_H),
                clipped=[chr(c) for c in clipped])
    return cells, info


if __name__ == '__main__':
    cells, info = pcf_cells(sys.argv[1])
    print("INFO:", info)
    for ch in "Agij_az0M@~?{}|":
        print(f"\n{ch!r}:")
        for r in cells[ord(ch)]:
            print("  " + "".join('#' if r & (1 << (7 - x)) else '.'
                                 for x in range(8)))

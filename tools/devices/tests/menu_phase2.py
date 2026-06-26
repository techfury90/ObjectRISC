#!/usr/bin/env python3
"""menu_phase2.py — render the pinnable OPEN LOOK Workspace menu headless.

oriscwm's desktop right-click menu (Phase 2) is a titled, pinnable menu: a
bold luBS "Workspace" title + the olgl pushpin (OUT sprite) above an olgx
text-ledge separator, then the items (one shown highlighted as the recessed
stadium).  The live framebuffer is Tk-only, so this mirror drives the SAME
ObjBlitGlyphs primitive against the SAME baked faces (luRS / luBS / olgl) the
WM uses and writes a 4x PNG — the headless twin of right-clicking the desktop.
Twin of menu_capsule.py / font_specimen.py.

Usage:  python3 tools/devices/tests/menu_phase2.py  # -> /tmp/orisc-menu-phase2.png"""
import importlib.machinery, importlib.util, sys, struct, zlib
from pathlib import Path
from types import SimpleNamespace

ROOT = Path("/Users/lando/ObjectRISC"); sys.path.insert(0, str(ROOT / "tools"))
L = importlib.machinery.SourceFileLoader("simorisc", str(ROOT / "tools/sim/simorisc"))
sim = importlib.util.module_from_spec(importlib.util.spec_from_loader("simorisc", L)); L.exec_module(sim)
from gen_wm_font import face_blob
BDF = "/Users/lando/Downloads/OpenLookCDROM-master/src/lib/xview3.2p1-X11R6-LinuxElf/fonts/bdf"
PID = 7
WHITE, BG1, BG2, BG3, BLACK, BLUE = 11, 10, 12, 13, 14, 9
ENDCAP, ITEM_H = 9, 20

def D(s, tt=0, w=0, h=0): return SimpleNamespace(live=True, generation=1, type_tag=tt, storage=s, length=len(s), fb_width=w, fb_height=h, fb_dirty=False)
def R(i, c): return sim.make_ref(generation=1, home=PID, index=i, caps=c)
class C:
    def __init__(s, d): s.pid=PID; s.descriptors=d; s._o={}; s._g={}
    def get_opr(s,i): return s._o.get(i,0)
    def set_opr(s,i,v): s._o[i]=v
    def get_gpr(s,i): return s._g.get(i,0)
    def set_gpr(s,i,v): s._g[i]=v

luRS, _ = face_blob(f"{BDF}/75dpi/luRS12.bdf", 32, 95, (12,16), True)
luBS, ib = face_blob(f"{BDF}/75dpi/luBS12.bdf", 32, 95, (12,16), True)   # bold, auto-fit
olgl, _ = face_blob(f"{BDF}/misc/olgl12.bdf", 19, 167, (47,47), False)
print("luBS cell", ib["cell_w"], "x", ib["cell_h"])
F_RS, F_BS, F_OLGL, F_TEXT = 2, 3, 4, 5
W, H = 170, 150
fb = bytearray([BLUE]*W*H)
descs = [None, D(fb, sim.TAG_FRAMEBUFFER, W, H), D(bytearray(luRS)), D(bytearray(luBS)), D(bytearray(olgl)), D(bytearray(b""))]
def cpu(): c=C(descs); c.set_opr(1, R(1, sim.CAP_R|sim.CAP_W)); return c
def fill(x,y,w,h,col):
    if w<=0 or h<=0: return
    c=cpu(); c.set_gpr(4,((x&0xFFFF)<<16)|(y&0xFFFF)); c.set_gpr(5,((w&0xFFFF)<<16)|(h&0xFFFF)); c.set_gpr(6,col); sim.primitive_ObjFillRect(c)
def blit(face, x, y, s, fg, bg=BG1):
    b = s if isinstance(s,(bytes,bytearray)) else s.encode()
    descs[F_TEXT]=D(bytearray(b)); c=cpu(); c.set_opr(2,R(face,sim.CAP_R)); c.set_opr(3,R(F_TEXT,sim.CAP_R))
    c.set_gpr(4,((x&0xFFFF)<<16)|(y&0xFFFF)); c.set_gpr(5,0xC0000000|(len(b)<<16)|(fg<<8)|bg); c.set_gpr(6,0); c.set_gpr(7,0); sim.primitive_ObjBlitGlyphs(c)

# measure proportional width from the face blob's width table (gen_wm_font layout)
def measure(face_blob_bytes, info, s):
    base, n = 32, 95
    wt_off = 24                      # header: magic..bpr = 6 words
    return sum(face_blob_bytes[wt_off + (ord(ch)-base)] for ch in s if base <= ord(ch) < base+n)
luBS_bytes = bytes(luBS); luRS_bytes = bytes(luRS)

def capsule(x, y, w):
    mx, mw, rx = x+ENDCAP, w-2*ENDCAP, x+w-ENDCAP
    fill(mx,y,mw,ITEM_H,BG2); blit(F_OLGL,x,y,bytes([26]),BG2); blit(F_OLGL,rx,y,bytes([29]),BG2)
    fill(mx,y,mw,1,BG3); blit(F_OLGL,x,y,bytes([24]),BG3); blit(F_OLGL,rx,y,bytes([28]),BG3)
    fill(mx,y+ITEM_H-2,mw,1,WHITE); blit(F_OLGL,x,y,bytes([25]),WHITE); blit(F_OLGL,rx,y,bytes([27]),WHITE)

def pushpin_out(x, y):              # 26x12, 3-layer emboss
    blit(F_OLGL, x, y, bytes([102]), BG2)    # middle / fill
    blit(F_OLGL, x, y, bytes([100]), WHITE)  # top / highlight
    blit(F_OLGL, x, y, bytes([101]), BG3)    # bottom / shadow

def text_ledge(x, y, w):            # olgx 2px raised box = white top + BG3 bottom
    fill(x, y, w, 1, WHITE); fill(x, y+1, w, 1, BG3)

# --- compose the menu -------------------------------------------------------
TITLE = "Workspace"
B, TMARGIN = 2, 3
PP_W, PP_LEFT, PP_TOP, TITLE_ROW_H = 26, 13, 3, 18
title_w = measure(luBS_bytes, ib, TITLE)
labels = ["Shell", "Edit", "Mouse Paint", "Menu Demo", "Font Demo"]
maxlabel = max(measure(luRS_bytes, None, s) for s in labels)
inner = max(maxlabel + 2*ENDCAP, PP_LEFT + PP_W + 4 + 3 + title_w)
mw = 2*B + inner
mh = 2*B + TITLE_ROW_H + 2 + len(labels)*ITEM_H
mx, my = 24, 16
# plate + raised frame
fill(mx, my, mw, mh, BG1)
fill(mx,my,mw,1,WHITE); fill(mx,my,1,mh,WHITE); fill(mx,my+mh-1,mw,1,BG3); fill(mx+mw-1,my,1,mh,BG3)
# title row: pushpin + centred bold title + ledge
pushpin_out(mx + PP_LEFT, my + PP_TOP)
tx0 = mx + PP_LEFT + PP_W + 4
title_x = tx0 + ((mx+mw-B) - tx0 - title_w)//2
blit(F_BS, title_x, my + B + (TITLE_ROW_H - ib["cell_h"])//2, TITLE, BLACK)
text_ledge(mx + B + TMARGIN, my + B + TITLE_ROW_H, mw - 2*B - 2*TMARGIN)
# items (3rd highlighted)
items_y0 = my + B + TITLE_ROW_H + 2
for i, name in enumerate(labels):
    iy, ix, iw = items_y0 + i*ITEM_H, mx+B, mw-2*B
    fill(ix, iy, iw, ITEM_H, BG1)
    hi = (i == 2)
    if hi: capsule(ix, iy, iw)
    blit(F_RS, ix+ENDCAP, iy+2, name, BLACK, BG2 if hi else BG1)

lut = sim._build_palette_lut(); Z = 4
rows = bytearray()
for y in range(H):
    scan = bytearray()
    for x in range(W): scan += bytes(lut[fb[y*W+x]])*Z
    for _ in range(Z): rows.append(0); rows += scan
def chunk(t,d): body=t+d; return struct.pack(">I",len(d))+body+struct.pack(">I",zlib.crc32(body)&0xffffffff)
out = "/tmp/orisc-menu-phase2.png"
with open(out,"wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n"); f.write(chunk(b"IHDR",struct.pack(">IIBBBBB",W*Z,H*Z,8,2,0,0,0)))
    f.write(chunk(b"IDAT",zlib.compress(bytes(rows),9))); f.write(chunk(b"IEND",b""))
print("wrote", out, W*Z, "x", H*Z)

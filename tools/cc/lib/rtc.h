/*
 * rtc.h — the Rich Text Control's layout core (v1, headless): lay out a
 * sequence of document Blocks into a flat DISPLAY LIST of positioned,
 * word-wrapped lines, using a per-glyph advance table. This is the terminal-
 * local, font-aware projection the doc-architecture calls for: the object
 * Document is a font-independent content graph, and layout (advances + wrap)
 * is its terminal-local view. It's mdview's local-wrap recipe applied to the
 * object document model.
 *
 * HANDLE-BASED by design. rtc_layout holds obj.h handles (ints), never
 * `void *__or` values, so it is ordinary C — arrays, `&local`, and
 * obj_fetch_to_stack all work with no OBJSTORE frame (which would miscompile
 * its int locals; see the pcc-or-frame note). A caller bridges the doc
 * model's `__or` block refs to handles (objor_stash_o7 + obj_adopt_o7) before
 * calling. Each block's bytes are pulled into a stack buffer via
 * obj_fetch_to_stack, so wrap reads the text like any char[] and sums cw[].
 *
 * The advance table `cw` has 256 entries: byte value -> pixel width (bulk-
 * loaded from the WM via wm_font_widths in the render slice; a stub table in
 * tests). Output is a module-global display list (rtc_nlines / rtc_line_*),
 * like mdview's item arrays.
 *
 * NOTE the 16-handle ceiling (OBJ_NHANDLE): pass only a VIEWPORT-bounded set
 * of block handles (a screenful + margin), not a whole large document — which
 * is exactly the freeze-on-scroll-away discipline (blocks outside the window
 * are frozen, not held live). Render + scroll + freeze are later slices.
 */

#ifndef RTC_H
#define RTC_H

#include "liborisc.h"
#include "obj.h"
#include "doc.h"      /* BLK_* kinds, BLOCK_HDR */

#define RTC_MAX_LINES  512

/* Lay out `nblocks` block HANDLES into the display list at wrap width `width`
 * (pixels) using advance table `cw` (256 entries: byte -> pixel width).
 * Returns the number of display lines (also rtc_nlines()), or -1 on a fetch
 * error / line-table overflow. Handles are read-only (not consumed). */
int rtc_layout(const obj_t *blocks, int nblocks, int width, const int *cw);

/* Wrap a single normal-memory text into the display list (as block 0, kind
 * BLK_PARA, from y=0), resetting the list first. This is the wrap algorithm
 * on its own — the object-free heart of rtc_layout — for testing and for
 * callers that already hold text in a buffer. Returns line count or -1. */
int rtc_wrap_text(const char *text, int len, int width, const int *cw);

int rtc_nlines(void);        /* display lines produced by the last layout */
int rtc_total_height(void);  /* total content height in pixels */
int rtc_line_block(int i);   /* source block index of line i */
int rtc_line_off(int i);     /* byte offset of line i within its block's text */
int rtc_line_len(int i);     /* byte length of line i */
int rtc_line_y(int i);       /* content-space top y of line i */
int rtc_line_kind(int i);    /* BLK_* kind of line i's block */

#endif /* RTC_H */

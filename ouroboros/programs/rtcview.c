/*
 * rtcview.c — render a native Document on screen (the RTC render app).
 *
 * The object-model counterpart of mdview: instead of parsing a Markdown file
 * into a flat doc[] byte array, rtcview builds a Document from the doc model
 * (Block objects via block_from_mem), lays it out with the Rich Text Control
 * (rtc_layout — walk the blocks, fetch each into a stack buffer, word-wrap
 * against a WM advance table into a display list), and draws the visible lines
 * with vec_text. Scrolling is mdview's OPEN LOOK loop: an owned scroll offset
 * over the display list, driven by keyboard and the WM scrollbar — the viewer
 * blocks on keyboard + pointer at once via obj_waitset_*, reports its scroll
 * state back with wm_set_scroll so the cable car tracks, and moves the content
 * on a scrollbar PTR_EVT_SCROLL push.
 *
 * SHELL/MENU-SPAWNED, mdview's twin: wm_open_session + term_init, then the same
 * loop — only the document SOURCE differs (a doc-model Document built with
 * block_from_mem, instead of a Markdown file parsed into a flat byte array).
 *
 * The RTC is HANDLE-based (obj.h handles, no `void *__or`), so render() is
 * ordinary C. The Document is built and its blocks bridged to handles in one
 * helper (build_doc) that isolates the `__or` work; after that the app uses
 * only handles.
 */

#include "liborisc.h"
#include "obj.h"       /* handles + obj_waitset_* */
#include "obj_or.h"    /* the __or block refs, bridged to handles */
#include "doc.h"
#include "rtc.h"

#define CONTENT_W   640
#define CONTENT_H   384
#define MARGIN_X    8
#define TEXT_W      (CONTENT_W - 2 * MARGIN_X)
#define TOP_Y       6
#define LINE_CAP    256          /* one drawn line's scratch */
#define BLOCK_CAP   1024         /* one block's bytes (header + text) */
#define MAX_BLOCKS  16           /* viewport-bounded (OBJ_NHANDLE ceiling) */

/* OPEN LOOK gray-group palette (VEC_PALETTE_HEX), as mdview. */
#define COL_PAPER   11
#define COL_INK     14
#define COL_RULE    12

#define STACK_BOTTOM 0x001f0000

static obj_t g_handles[MAX_BLOCKS];
static int   g_count;
static int   cw[256];

/* After layout, every block's text is copied here and the block handles are
 * dropped, so render() slices from normal memory (like mdview's doc[]) and the
 * 16-slot handle table is free for the pointer surface (the scrollbar). */
static char  g_docbuf[8192];
static int   g_boff[MAX_BLOCKS];   /* each block's text offset within g_docbuf */

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}
#define WP(s) do { restore_or_state(); print_str(s); } while (0)

static int
should_quit(int code)
{
	return code == 'q' || code == 'Q' || code == TK_ESCAPE;
}

/* Map a scroll key to a new clamped offset; -1 requests quit. */
static int
scroll_key(int code, int offset, int maxoff)
{
	int noff = offset;
	if (should_quit(code)) return -1;
	if      (code == TK_DOWN)      noff += 18;
	else if (code == TK_UP)        noff -= 18;
	else if (code == TK_PAGE_DOWN || code == ' ') noff += CONTENT_H - 18;
	else if (code == TK_PAGE_UP)   noff -= CONTENT_H - 18;
	else if (code == TK_HOME)      noff  = 0;
	else if (code == TK_END)       noff  = maxoff;
	if (noff < 0)      noff = 0;
	if (noff > maxoff) noff = maxoff;
	return noff;
}

/* Face + line height for a block kind. */
static int
kind_face(int kind)
{
	if (kind == BLK_H1 || kind == BLK_H2) return FONT_FACE_BOLD;
	if (kind == BLK_CODE) return FONT_FACE_MONO;
	return FONT_FACE_PROP;
}

/* Build the demo Document and bridge its blocks to handles in g_handles;
 * isolates all the `__or` work (never an int-param helper / &int_local here —
 * pcc-or-frame trap). Returns 0, or a negative error. */
/* The demo document as interned string globals, so block_from_mem gets each
 * length from sizeof — never a hand-counted literal (that truncated the text
 * on screen). Enough blocks that the content exceeds the window (scrollable). */
static const char s_title[] = "Object RISC";
static const char s_p1[] =
    "The document you are reading is not a file on a disk. It is a live graph "
    "of capability objects, held in the very same object memory the rest of "
    "the operating system runs in, and it is being rendered on screen right "
    "now by the terminal-local Rich Text Control that is drawing these words. "
    "There is no document format to parse and no viewer separate from the "
    "system itself.";
static const char s_p2[] =
    "Each paragraph, heading, and rule is a first-class Block object with its "
    "own unforgeable name. The viewport you are looking at was laid out by "
    "fetching a glyph-advance table from the window manager exactly once, and "
    "then word-wrapping every visible line entirely on the terminal, summing "
    "those advances locally with no per-word round trip back to the display "
    "server for measurement.";
static const char s_h2[] = "Freeze on scroll away";
static const char s_p3[] =
    "A real document may contain far more blocks than can usefully be kept "
    "live at any one moment. Blocks that scroll out of the viewport are "
    "serialised into a compact byte log and then freed, so the count of live "
    "objects stays close to the size of the window even as the document itself "
    "grows without any bound at all. Scrolling one of them back into view "
    "simply thaws it again from the log.";
static const char s_h3[] = "Capabilities all the way down";
static const char s_p4[] =
    "Every block is named by a capability that cannot be forged or guessed. "
    "Sharing a block with another reader is nothing more than granting them a "
    "reference to it, and revoking that access again is a single generation "
    "bump in the object's descriptor. There is no ambient authority and no "
    "access-control list anywhere in the graph; the reference you happen to "
    "hold is precisely and only the permission that you have.";
static const char s_p5[] =
    "This very layout was produced by walking the block array in order, "
    "bridging each block's capability into a short-lived handle, fetching its "
    "raw bytes into a stack buffer, and word-wrapping the resulting text "
    "against the proportional Lucida Sans advances that the window manager "
    "measured directly from its baked bitmap font at start up.";
static const char s_p6[] =
    "Scrolling simply moves a viewport offset across the resulting display "
    "list of positioned lines. Only the blocks that actually intersect the "
    "viewport need to be live at any given instant, which is exactly the "
    "discipline that freeze on scroll away is there to enforce, and exactly "
    "the reason a capability document can be arbitrarily large and yet remain "
    "cheap to hold open and to scroll through.";

static int
build_doc(void)
{
	void *__or doc;
	void *__or blocks;
	void *__or b;
	int i, count;

	doc = doc_new(4);
	if (objor_isnull(doc))
		return -1;
	count = 0;

	/* A macro, NOT a helper: an int-param function that also holds `__or`
	 * miscompiles under pcc (the OBJSTORE-frame trap). sizeof(s)-1 is the exact
	 * byte length, a compile-time constant. */
#define ADD(k, sty, s) do {                                 \
		b = block_from_mem((k), (sty), (s), sizeof(s) - 1); \
		if (objor_isnull(b)) return -2;                     \
		blocks = orvec_push(doc_blocks(doc), b, count);     \
		if (objor_isnull(blocks)) return -3;                \
		doc_set_blocks(doc, blocks); count++;               \
	} while (0)

	ADD(BLK_H1,   0, s_title);
	ADD(BLK_PARA, 0, s_p1);
	ADD(BLK_PARA, 0, s_p2);
	ADD(BLK_H2,   0, s_h2);
	ADD(BLK_PARA, 0, s_p3);
	ADD(BLK_H2,   0, s_h3);
	ADD(BLK_PARA, 0, s_p4);
	ADD(BLK_PARA, 0, s_p5);
	ADD(BLK_PARA, 0, s_p6);
#undef ADD

	/* bridge each block ref to a handle for the handle-based layout/render */
	blocks = doc_blocks(doc);
	for (i = 0; i < count; i++) {
		objor_stash_o7(objor_vget(blocks, i));
		g_handles[i] = obj_adopt_o7();
		if (obj_isnull(g_handles[i]))
			return -4;
	}
	g_count = count;
	return 0;
}

/* Bulk-load the proportional face's advances into cw[] (~6 WM calls). */
static void
prewarm_widths(int face)
{
	unsigned char buf[16];
	int start, i;
	for (start = 32; start < 128; start += 16) {
		if (wm_font_widths(face, start, buf) != 0) return;
		for (i = 0; i < 16; i++)
			cw[start + i] = buf[i] ? buf[i] : 1;
	}
}

/* Copy every block's text into g_docbuf and DROP its handle. Fetches each
 * block's bytes into a stack buffer (obj_fetch_to_stack) and appends the text
 * region to g_docbuf, recording the per-block offset; obj_drop frees the handle
 * slot (the block object itself stays live in the doc's orvec). Returns 0. */
static int
cache_doc(void)
{
	char tmp[BLOCK_CAP];
	int i;

	g_boff[0] = 0;
	{ int d = 0;
	for (i = 0; i < g_count; i++) {
		int bytelen = obj_len(g_handles[i]);
		int off = (int)((unsigned int)tmp - STACK_BOTTOM);
		int text_len, j;

		if (bytelen > BLOCK_CAP) bytelen = BLOCK_CAP;
		if (obj_fetch_to_stack(g_handles[i], off, bytelen) != 0)
			return -1;
		text_len = bytelen - BLOCK_HDR;
		if (text_len < 0) text_len = 0;
		g_boff[i] = d;
		for (j = 0; j < text_len && d < (int)sizeof(g_docbuf); j++)
			g_docbuf[d++] = tmp[BLOCK_HDR + j];
		obj_drop(g_handles[i]);
	}
	}
	return 0;
}

/* Draw the display lines visible at scroll `offset`, slicing each line's span
 * out of g_docbuf (normal memory) — no objects, no handles. */
static void
render(int offset)
{
	char line[LINE_CAP];
	int n = rtc_nlines();
	int i;

	vec_set_color(COL_PAPER);
	vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	for (i = 0; i < n; i++) {
		int sy = rtc_line_y(i) - offset;
		int kind, base, len, j;

		if (sy + 24 <= 0 || sy >= CONTENT_H)
			continue;                       /* off-screen */
		kind = rtc_line_kind(i);
		if (kind == BLK_RULE) {
			vec_set_color(COL_RULE);
			vec_line(MARGIN_X, sy, CONTENT_W - MARGIN_X - 1, sy);
			continue;
		}
		base = g_boff[rtc_line_block(i)] + rtc_line_off(i);
		len = rtc_line_len(i);
		if (len > LINE_CAP - 1) len = LINE_CAP - 1;
		for (j = 0; j < len; j++)
			line[j] = g_docbuf[base + j];
		line[len] = 0;
		vec_set_color(COL_INK);
		vec_text(kind_face(kind), MARGIN_X, sy, line);
	}
}

int
main(void)
{
	int wid = 0;
	int rc, total, maxoff, offset, have_ptr, nq;
	obj_t qs[2];
	obj_t waitset;

	task_init();

	rc = wm_open_session("Rich Text Control", &wid);
	if (rc != 0) { WP("rtcview: wm_open_session failed\n"); return rc; }
	term_init();

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { WP("rtcview: bind VECTOR failed\n"); goto out; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { WP("rtcview: vec_init failed\n"); goto out; }

	if (build_doc() != 0) { WP("rtcview: build_doc failed\n"); rc = -1; goto out; }

	/* rtc_layout wraps against ONE advance table; use the body (proportional)
	 * face — headings are short and never wrap, so bold's slightly wider
	 * glyphs don't matter. (Prewarming several faces here would just overwrite
	 * cw[], since it is a single table, not per-face like mdview's.) */
	prewarm_widths(FONT_FACE_PROP);

	if (rtc_layout(g_handles, g_count, TEXT_W, cw) < 0) {
		WP("rtcview: rtc_layout failed\n"); rc = -1; goto out;
	}
	/* Cache the text + release the block handles BEFORE binding the pointer
	 * surface, so the 16-slot handle table has room for it (the scrollbar). */
	if (cache_doc() != 0) { WP("rtcview: cache_doc failed\n"); rc = -1; goto out; }
	total = rtc_total_height() + TOP_Y;
	maxoff = total - CONTENT_H;
	if (maxoff < 0) maxoff = 0;

	offset = 0;
	render(offset);
	wm_set_scroll(wid, total, offset);

	/* Pointer surface for the WM scrollbar (best-effort; keyboard still works). */
	have_ptr = (wm_bind_surface(wid, WSURF_POINTER) == 0
	            && pointer_init_from_dir_result() == 0
	            && pointer_subscribe() == 0);

	nq = 0;
	qs[nq++] = term_kbd_mbox();
	if (have_ptr) qs[nq++] = pointer_event_mbox();
	waitset = obj_waitset_new(qs, nq);

	if (waitset < 0) {
		/* keyboard-only blocking fallback */
		for (;;) {
			int m = 0, n = scroll_key(term_getkey(&m), offset, maxoff);
			if (n < 0) break;
			if (n != offset) {
				offset = n; render(offset);
				wm_set_scroll(wid, total, offset);
			}
		}
		goto out;
	}

	for (;;) {
		int code = 0, mods = 0, noff = offset, quit = 0;

		obj_waitset_wait(waitset, nq, 0);      /* block until a source is ready */

		while (term_pollkey(&code, &mods) == 0) {   /* drain + coalesce keys */
			int n = scroll_key(code, noff, maxoff);
			if (n < 0) { quit = 1; break; }
			noff = n;
		}
		if (quit) break;
		if (noff != offset) {
			offset = noff; render(offset);
			wm_set_scroll(wid, total, offset);      /* keyboard moves the elevator */
		}

		if (have_ptr) {                              /* scrollbar pushes move content */
			int et = 0, pxy = 0, btn = 0, bs = 0, got = 0, soff = offset;
			while (pointer_getevent(&et, &pxy, &btn, &bs) == 0)
				if (et == PTR_EVT_SCROLL) { got = 1; soff = pxy; }
			if (got) {
				if (soff < 0)      soff = 0;
				if (soff > maxoff) soff = maxoff;
				if (soff != offset) { offset = soff; render(offset); }
			}
		}
	}

out:
	term_shutdown();
	wm_destroy_window(wid);
	return rc < 0 ? -rc : rc;
}

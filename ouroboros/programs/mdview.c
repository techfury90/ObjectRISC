/*
 * mdview.c — the Ouroboros Markdown viewer (the north-star GUI app).
 *
 * The document is parsed into a display list of positioned items (text lines,
 * rules, code panels, bullet discs), each carrying its content-space y.  Text
 * items reference the source by (offset,len) into doc[] — NO copy — and words
 * are word-wrapped against a local glyph-width table bulk-fetched once via
 * WM_OP_FONT_WIDTHS, each measured a single time (layout is one linear pass).
 * Layout is VIEWPORT-FIRST: layout_upto() lays out just the first screen, paints
 * it, then finishes the tail — the reader sees text as soon as the visible
 * region is ready.  The viewer owns a scroll OFFSET; render() culls to the
 * visible items and slices each line's span out of doc[] at draw time.
 *
 * Scrolling: keyboard (arrows / page / space / Home / End) AND the WM's OPEN
 * LOOK scrollbar drive the offset — the viewer blocks on keyboard + pointer at
 * once (obj_waitset_*) and reports its scroll state back so the cable car tracks
 * (wm_set_scroll), while scrollbar pushes (PTR_EVT_SCROLL) move the content.
 *
 * Shell/menu-spawned (edit/font_demo shape): wm_open_session + term_init +
 * hf_init, lay out the first screen, draw, then loop on input.
 */

#include "liborisc.h"
#include "obj.h"     /* obj_waitset_* — block on keyboard + scrollbar at once */

#define CONTENT_W   640
#define CONTENT_H   384
#define MARGIN_X    8
#define TEXT_W      (CONTENT_W - 2 * MARGIN_X)   /* proportional wrap width */
#define BULLET_IND  18                           /* bullet text indent */
#define LH          18                           /* line height */

/* OPEN LOOK gray-group palette (VEC_PALETTE_HEX). */
#define COL_PAPER   11   /* White  #f5f5f5 — the page          */
#define COL_INK     14   /* Black  #000000 — text              */
#define COL_RULE    12   /* BG2    #b8b8b8 — rule + bullet disc */
#define COL_CODEBG  10   /* BG1    #cccccc — code-block panel   */

#define DOC_PATH    "/docs/welcome.md"
#define DOC_MAX     8192

/* Display list: the parsed document as positioned draw items (content-space y).
 * Text items reference the source by (offset,len) into doc[] — NO copy; render
 * slices the span out at draw time, for visible items only.  Words are measured
 * once at layout (their widths feed the wrap), so layout is one pass over the
 * text with no per-word re-summing and no line copies. */
#define MAX_ITEMS   128
#define LINE_CAP    256   /* render scratch for one display line's span */
#define I_TEXT      0    /* face,x,y,off,len — a wrapped text line       */
#define I_CODE      1    /* x,y,off,len — a code line (mono on a panel)  */
#define I_RULE      2    /* y — an H1 hairline rule                      */
#define I_DISC      3    /* x,y — a bullet disc                          */

static char doc[DOC_MAX];
static int  doc_len;

static int  it_kind[MAX_ITEMS];
static int  it_face[MAX_ITEMS];
static int  it_x[MAX_ITEMS];
static int  it_y[MAX_ITEMS];            /* content-space top y */
static int  it_off[MAX_ITEMS];          /* byte offset into doc[] (I_TEXT/I_CODE) */
static int  it_len[MAX_ITEMS];          /* byte length (0 for I_RULE/I_DISC) */
static int  n_items;
static int  total_px;                   /* full content height */

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

/* Read `path` into doc[] (NUL-terminated).  hf_read needs a stack buffer. */
static int
load_doc(const char *path)
{
	if (hf_init() != 0) return -1;
	int fd = hf_open(path, HF_O_RDONLY);
	if (fd < 0) return fd;

	char chunk[4096];   /* one host read()/round-trip per 4 KB (vs 256 B) — a
	                     * whole welcome.md-sized doc loads in a single read */
	int n;
	doc_len = 0;
	while ((n = hf_read(fd, chunk, sizeof(chunk))) > 0) {
		int i;
		for (i = 0; i < n && doc_len < DOC_MAX - 1; i++)
			doc[doc_len++] = chunk[i];
	}
	hf_close(fd);
	doc[doc_len] = 0;
	return 0;
}

/* Append one display-list item.  Text items carry a (off,len) slice of doc[]
 * (off/len = 0 for the textless rule/disc); no string is copied. */
static void
emit(int kind, int face, int x, int y, int off, int len)
{
	if (n_items >= MAX_ITEMS) return;
	it_kind[n_items] = kind;
	it_face[n_items] = face;
	it_x[n_items]    = x;
	it_y[n_items]    = y;
	it_off[n_items]  = off;
	it_len[n_items]  = len;
	n_items++;
}

/* Per-face, per-byte glyph-advance cache (0 = unmeasured).  Only the WM holds
 * the proportional widths, but a round-trip PER WORD makes loading a document
 * unbearably slow on the simulator.  So we pre-load each face's printable-ASCII
 * advances in bulk (prewarm_widths below, ~6 WM calls/face via WM_OP_FONT_WIDTHS)
 * and thereafter measure any string locally — sum of advances, exactly what the
 * WM's font_measure does byte by byte, so wrap stays consistent with the
 * vec_text rendering.  text_width's per-byte wm_measure_text is now just the
 * lazy fallback for a codepoint prewarm didn't cover (e.g. >= 128).  Net: ~18
 * round-trips at load instead of ~350+, and scroll re-renders need none. */
static int cw[4][256];

static int
text_width(int face, const char *s, int n)
{
	int total = 0, i;
	if (face < 0 || face >= 4) return 0;
	for (i = 0; i < n; i++) {
		int c = (unsigned char)s[i];
		if (cw[face][c] == 0) {
			char tmp[2];
			tmp[0] = (char)c; tmp[1] = 0;
			int w = wm_measure_text(face, tmp);
			cw[face][c] = (w > 0) ? w : 1;   /* >=1 so the slot reads "measured" */
		}
		total += cw[face][c];
	}
	return total;
}

/* Bulk-load `face`'s printable-ASCII advances into cw[] in ~6 WM calls, so the
 * subsequent layout measures entirely locally (no per-word round-trips). */
static void
prewarm_widths(int face)
{
	unsigned char buf[16];
	int start, i;
	for (start = 32; start < 128; start += 16) {
		if (wm_font_widths(face, start, buf) != 0) return;
		for (i = 0; i < 16; i++)
			cw[face][start + i] = buf[i] ? buf[i] : 1;
	}
}

/* Greedy word-wrap `text` (a span of doc[]) in `face` into I_TEXT items at
 * content-y `y`.  Words are contiguous in the source, so a wrapped line is just
 * the doc[] slice from its first word to its last — tracked as (line_off,
 * line_end), never copied.  Each word is measured ONCE and the running line
 * width is kept, so layout is one linear pass.  Returns the y past the last line. */
static int
wrap_emit(int face, const char *text, int x, int y, int max_w)
{
	int  space_w  = text_width(face, " ", 1);   /* measured once (cached) */
	int  line_off = -1, line_end = 0, line_w = 0;
	const char *p = text;

	while (*p) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *ws = p;
		while (*p && *p != ' ') p++;
		int wl = (int)(p - ws);

		int word_w = text_width(face, ws, wl);
		int sep    = (line_off >= 0) ? space_w : 0;

		if (line_off >= 0 && line_w + sep + word_w > max_w) {
			emit(I_TEXT, face, x, y, line_off, line_end - line_off); y += LH;
			line_off = -1; line_w = 0; sep = 0;
		}
		if (line_off < 0) line_off = (int)(ws - doc);   /* line starts at this word */
		line_end = (int)(p - doc);                      /* ...and now ends after it */
		line_w  += sep + word_w;
	}
	if (line_off >= 0) { emit(I_TEXT, face, x, y, line_off, line_end - line_off); y += LH; }
	return y;
}

/* Resumable layout state for viewport-first layout: lay out only as far as the
 * first paint needs, then finish the rest.  lay_p is the resume cursor. */
static char *lay_p;
static int   lay_y;
static int   lay_incode;
static int   lay_done;

static void
layout_reset(void)
{
	n_items    = 0;
	lay_y      = 6;
	lay_incode = 0;
	lay_p      = doc;
	lay_done   = 0;
	total_px   = lay_y;
}

/* Lay out more of the document until content-y reaches `target_y` (or the doc
 * ends), resuming from the previous call.  Splits each source line IN PLACE
 * (NUL-terminating at the newline) and appends (offset,len) display items; words
 * are measured once by wrap_emit.  Maintains lay_done + the running total_px. */
static void
layout_upto(int target_y)
{
	char *p = lay_p;
	int y = lay_y, in_code = lay_incode;

	while (*p && y < target_y) {
		char *ln = p;
		while (*p && *p != '\n') p++;
		int linelen = (int)(p - ln);
		if (*p == '\n') { *p = 0; p++; }

		if (ln[0] == '`' && ln[1] == '`' && ln[2] == '`') { in_code = !in_code; continue; }
		if (in_code) { emit(I_CODE, FONT_FACE_MONO, MARGIN_X, y, (int)(ln - doc), linelen); y += LH; continue; }
		if (ln[0] == 0) { y += 8; continue; }

		if (ln[0] == '#') {
			int lv = 0;
			while (ln[lv] == '#') lv++;
			const char *h = ln + lv;
			while (*h == ' ') h++;
			y = wrap_emit(FONT_FACE_BOLD, h, MARGIN_X, y, TEXT_W);
			if (lv == 1) { emit(I_RULE, 0, MARGIN_X, y, 0, 0); y += 8; }
			else         { y += 4; }
			continue;
		}

		if ((ln[0] == '-' || ln[0] == '*') && ln[1] == ' ') {
			emit(I_DISC, 0, MARGIN_X, y, 0, 0);
			y = wrap_emit(FONT_FACE_PROP, ln + 2, MARGIN_X + BULLET_IND, y, TEXT_W - BULLET_IND);
			continue;
		}

		y = wrap_emit(FONT_FACE_PROP, ln, MARGIN_X, y, TEXT_W);
	}

	lay_p      = p;
	lay_y      = y;
	lay_incode = in_code;
	if (!*p) lay_done = 1;
	total_px   = y;
}

/* Draw the items visible at scroll `offset` (a cheap cull — no re-wrap). */
static void
render(int offset)
{
	vec_set_color(COL_PAPER);
	vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	char buf[LINE_CAP];
	int i;
	for (i = 0; i < n_items; i++) {
		int sy = it_y[i] - offset;
		if (sy + LH <= 0 || sy >= CONTENT_H) continue;   /* off-screen */
		int k = it_kind[i];
		if (k == I_TEXT || k == I_CODE) {
			/* slice this line's doc[] span into a NUL-terminated draw buffer */
			int len = it_len[i];
			if (len > LINE_CAP - 1) len = LINE_CAP - 1;
			int j;
			for (j = 0; j < len; j++) buf[j] = doc[it_off[i] + j];
			buf[len] = 0;
			if (k == I_TEXT) {
				vec_set_color(COL_INK);
				vec_text(it_face[i], it_x[i], sy, buf);
			} else {
				vec_set_color(COL_CODEBG);
				vec_rect_fill(MARGIN_X, sy - 2, TEXT_W, LH);
				vec_set_color(COL_INK);
				vec_text(FONT_FACE_MONO, it_x[i] + 4, sy, buf);
			}
		} else if (k == I_RULE) {
			vec_set_color(COL_RULE);
			vec_line(it_x[i], sy, CONTENT_W - MARGIN_X - 1, sy);
		} else if (k == I_DISC) {
			vec_set_color(COL_RULE);
			vec_oval_fill(it_x[i] + 4, sy + 5, 5, 5);
		}
	}
}

/* Map a scroll key to a new clamped offset; returns -1 to request quit. */
static int
scroll_key(int code, int offset, int maxoff)
{
	if (should_quit(code)) return -1;
	int noff = offset;
	if      (code == TK_DOWN)                     noff += LH;
	else if (code == TK_UP)                       noff -= LH;
	else if (code == TK_PAGE_DOWN || code == ' ') noff += CONTENT_H - LH;
	else if (code == TK_PAGE_UP)                  noff -= CONTENT_H - LH;
	else if (code == TK_HOME)                     noff  = 0;
	else if (code == TK_END)                      noff  = maxoff;
	if (noff < 0)      noff = 0;
	if (noff > maxoff) noff = maxoff;
	return noff;
}

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("Markdown Viewer", &wid);
	if (rc != 0) { WP("mdview: wm_open_session failed\n"); return rc; }

	term_init();

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { WP("mdview: bind VECTOR failed\n"); goto out; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { WP("mdview: vec_init failed\n");    goto out; }

	if (load_doc(DOC_PATH) != 0) {
		vec_set_color(COL_PAPER); vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);
		vec_set_color(COL_INK);
		vec_text(FONT_FACE_PROP, 8, 8, "mdview: could not open " DOC_PATH);
		for (;;) {
			int c = 0, m = 0;
			if (term_pollkey(&c, &m) == 0 && should_quit(c)) break;
			task_yield();
		}
		goto out;
	}

	prewarm_widths(FONT_FACE_PROP);
	prewarm_widths(FONT_FACE_BOLD);
	prewarm_widths(FONT_FACE_MONO);
	/* Viewport-first: lay out just the first screen, paint it, THEN finish the
	 * tail — the reader sees text as soon as the visible region is ready instead
	 * of after the whole document is wrapped.  (A big-doc mode would background-
	 * fill the tail on idle via the WaitAnyQueue timeout rather than finishing it
	 * synchronously here.) */
	layout_reset();
	layout_upto(CONTENT_H + LH);
	int offset = 0;
	render(offset);
	wm_set_scroll(wid, total_px, offset);
	layout_upto(0x7fffffff);                /* finish the tail; total_px now exact */
	int maxoff = total_px - CONTENT_H;
	if (maxoff < 0) maxoff = 0;
	wm_set_scroll(wid, total_px, offset);   /* refresh the cable car for full height */

	/* Bind a pointer surface + subscribe so the WM can PUSH scrollbar-driven
	 * scrolls (PTR_EVT_SCROLL) to us.  Best-effort: the keyboard still scrolls
	 * without it. */
	int have_ptr = (wm_bind_surface(wid, WSURF_POINTER) == 0
	                && pointer_init_from_dir_result() == 0
	                && pointer_subscribe() == 0);

	/* Block on the keyboard AND (if subscribed) the pointer mailbox at once via a
	 * WaitAnyQueue set — event-driven, no busy-poll.  Keyboard scrolls move the
	 * elevator (we report back with wm_set_scroll); scrollbar pushes move the
	 * content (the WM already moved the elevator, so we DON'T report back — no
	 * echo loop). */
	obj_t qs[2];
	int   nq = 0;
	qs[nq++] = term_kbd_mbox();
	if (have_ptr) qs[nq++] = pointer_event_mbox();
	obj_t waitset = obj_waitset_new(qs, nq);

	if (waitset < 0) {
		/* Wait-set alloc failed — keyboard-only blocking fallback. */
		for (;;) {
			int m = 0, n = scroll_key(term_getkey(&m), offset, maxoff);
			if (n < 0) break;
			if (n != offset) {
				offset = n; render(offset);
				wm_set_scroll(wid, total_px, offset);
			}
		}
		goto out;
	}

	for (;;) {
		obj_waitset_wait(waitset, nq, 0);   /* block until a source is ready */

		/* keyboard: drain all pending keys, coalesce into one render */
		int code = 0, mods = 0, noff = offset, quit = 0;
		while (term_pollkey(&code, &mods) == 0) {
			int n = scroll_key(code, noff, maxoff);
			if (n < 0) { quit = 1; break; }
			noff = n;
		}
		if (quit) break;
		if (noff != offset) {
			offset = noff; render(offset);
			wm_set_scroll(wid, total_px, offset);   /* keyboard moves the elevator */
		}

		/* pointer: drain scrollbar pushes, coalesce to the latest offset */
		if (have_ptr) {
			int et = 0, pxy = 0, btn = 0, bs = 0, got = 0, soff = offset;
			while (pointer_getevent(&et, &pxy, &btn, &bs) == 0)
				if (et == PTR_EVT_SCROLL) { got = 1; soff = pxy; }
			if (got) {
				if (soff < 0)      soff = 0;
				if (soff > maxoff) soff = maxoff;
				if (soff != offset) { offset = soff; render(offset); }
				/* no wm_set_scroll — the WM already positioned the elevator */
			}
		}
	}

out:
	term_shutdown();
	wm_destroy_window(wid);
	return rc < 0 ? -rc : rc;
}

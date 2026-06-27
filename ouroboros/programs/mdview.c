/*
 * mdview.c — the Ouroboros Markdown viewer (the north-star GUI app).
 *
 * Phase 3a: scrolling.  The document is parsed ONCE at load into a display list
 * of positioned items (text lines word-wrapped against a local glyph-width table
 * bulk-fetched via WM_OP_FONT_WIDTHS, plus rules / code panels / bullet discs),
 * each carrying its content-space y.  The
 * viewer owns a scroll OFFSET; render() draws only the items visible in the
 * window at that offset (cheap cull, no re-wrap), so keyboard scrolling
 * (arrows / page / space / Home / End) is smooth.  Reporting the offset to the
 * scrollbar (proportional elevator) and letting the scrollbar drive it are the
 * next steps (3b / 3c).
 *
 * Shell/menu-spawned (edit/font_demo shape): wm_open_session + term_init +
 * hf_init, build the layout, draw, then loop on keys.
 */

#include "liborisc.h"

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

/* Display list: the parsed document as positioned draw items (content-space y). */
#define MAX_ITEMS   128
#define TEXT_CAP    128   /* must exceed the ~95-char window wrap width + margin */
#define I_TEXT      0    /* face,x,y,text — a wrapped text line          */
#define I_CODE      1    /* x,y,text — a code line (mono on a panel)     */
#define I_RULE      2    /* y — an H1 hairline rule                      */
#define I_DISC      3    /* x,y — a bullet disc                          */

static char doc[DOC_MAX];
static int  doc_len;

static int  it_kind[MAX_ITEMS];
static int  it_face[MAX_ITEMS];
static int  it_x[MAX_ITEMS];
static int  it_y[MAX_ITEMS];            /* content-space top y */
static char it_text[MAX_ITEMS][TEXT_CAP];
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

	char chunk[256];
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

/* Append one display-list item. */
static void
emit(int kind, int face, int x, int y, const char *text)
{
	if (n_items >= MAX_ITEMS) return;
	it_kind[n_items] = kind;
	it_face[n_items] = face;
	it_x[n_items]    = x;
	it_y[n_items]    = y;
	int i = 0;
	if (text) while (text[i] && i < TEXT_CAP - 1) { it_text[n_items][i] = text[i]; i++; }
	it_text[n_items][i] = 0;
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

/* Greedy word-wrap `text` in `face` into I_TEXT items at content-y `y`, breaking
 * so each line measures <= max_w (text_width — locally summed from the cache).
 * Returns the y past the last line. */
static int
wrap_emit(int face, const char *text, int x, int y, int max_w)
{
	char line[TEXT_CAP], cand[TEXT_CAP];
	int  ll = 0;
	const char *p = text;

	line[0] = 0;
	while (*p) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *ws = p;
		while (*p && *p != ' ') p++;
		int wl = (int)(p - ws);

		int cl = 0, i;
		for (i = 0; i < ll; i++) cand[cl++] = line[i];
		if (ll > 0 && cl < TEXT_CAP - 1) cand[cl++] = ' ';
		for (i = 0; i < wl && cl < TEXT_CAP - 1; i++) cand[cl++] = ws[i];
		cand[cl] = 0;

		int w = text_width(face, cand, cl);
		if (w >= 0 && w <= max_w) {
			for (i = 0; i <= cl; i++) line[i] = cand[i];
			ll = cl;
		} else {
			if (ll > 0) { emit(I_TEXT, face, x, y, line); y += LH; }
			ll = 0;
			for (i = 0; i < wl && ll < TEXT_CAP - 1; i++) line[ll++] = ws[i];
			line[ll] = 0;
		}
	}
	if (ll > 0) { emit(I_TEXT, face, x, y, line); y += LH; }
	return y;
}

/* Parse doc[] into the display list (once), and set total_px. */
static void
build_layout(void)
{
	n_items = 0;
	int y = 6;
	int in_code = 0;
	char *p = doc;

	while (*p) {
		/* Split the next line IN PLACE: NUL-terminate at the newline so a full
		 * (possibly very long) paragraph reaches wrap_emit uncopied — there is no
		 * fixed line buffer to overflow.  doc[] is parsed once and unused after. */
		char *ln = p;
		while (*p && *p != '\n') p++;
		if (*p == '\n') { *p = 0; p++; }

		if (ln[0] == '`' && ln[1] == '`' && ln[2] == '`') { in_code = !in_code; continue; }
		if (in_code) { emit(I_CODE, FONT_FACE_MONO, MARGIN_X, y, ln); y += LH; continue; }
		if (ln[0] == 0) { y += 8; continue; }

		if (ln[0] == '#') {
			int lv = 0;
			while (ln[lv] == '#') lv++;
			const char *h = ln + lv;
			while (*h == ' ') h++;
			y = wrap_emit(FONT_FACE_BOLD, h, MARGIN_X, y, TEXT_W);
			if (lv == 1) { emit(I_RULE, 0, MARGIN_X, y, ""); y += 8; }
			else         { y += 4; }
			continue;
		}

		if ((ln[0] == '-' || ln[0] == '*') && ln[1] == ' ') {
			emit(I_DISC, 0, MARGIN_X, y, "");
			y = wrap_emit(FONT_FACE_PROP, ln + 2, MARGIN_X + BULLET_IND, y, TEXT_W - BULLET_IND);
			continue;
		}

		y = wrap_emit(FONT_FACE_PROP, ln, MARGIN_X, y, TEXT_W);
	}
	total_px = y;
}

/* Draw the items visible at scroll `offset` (a cheap cull — no re-wrap). */
static void
render(int offset)
{
	vec_set_color(COL_PAPER);
	vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	int i;
	for (i = 0; i < n_items; i++) {
		int sy = it_y[i] - offset;
		if (sy + LH <= 0 || sy >= CONTENT_H) continue;   /* off-screen */
		int k = it_kind[i];
		if (k == I_TEXT) {
			vec_set_color(COL_INK);
			vec_text(it_face[i], it_x[i], sy, it_text[i]);
		} else if (k == I_CODE) {
			vec_set_color(COL_CODEBG);
			vec_rect_fill(MARGIN_X, sy - 2, TEXT_W, LH);
			vec_set_color(COL_INK);
			vec_text(FONT_FACE_MONO, it_x[i] + 4, sy, it_text[i]);
		} else if (k == I_RULE) {
			vec_set_color(COL_RULE);
			vec_line(it_x[i], sy, CONTENT_W - MARGIN_X - 1, sy);
		} else if (k == I_DISC) {
			vec_set_color(COL_RULE);
			vec_oval_fill(it_x[i] + 4, sy + 5, 5, 5);
		}
	}
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
	build_layout();
	int maxoff = total_px - CONTENT_H;
	if (maxoff < 0) maxoff = 0;
	int offset = 0;
	render(offset);
	wm_set_scroll(wid, total_px, offset);   /* park the cable car at the top */

	/* Scroll on arrows / page / space / Home / End; quit on q / Esc.  Blocking
	 * term_getkey (edit.c's path — its arrow keys work; term_pollkey did not
	 * deliver the special keys reliably here).  3c will move to a non-blocking
	 * poll once the viewer also listens for scrollbar-driven scroll events. */
	for (;;) {
		int mods = 0;
		int code = term_getkey(&mods);
		if (should_quit(code)) break;
		int noff = offset;
		if      (code == TK_DOWN)                     noff += LH;
		else if (code == TK_UP)                       noff -= LH;
		else if (code == TK_PAGE_DOWN || code == ' ') noff += CONTENT_H - LH;
		else if (code == TK_PAGE_UP)                  noff -= CONTENT_H - LH;
		else if (code == TK_HOME)                     noff  = 0;
		else if (code == TK_END)                      noff  = maxoff;
		if (noff < 0)       noff = 0;
		if (noff > maxoff)  noff = maxoff;
		if (noff != offset) {
			offset = noff;
			render(offset);
			wm_set_scroll(wid, total_px, offset);   /* cable car follows */
		}
	}

out:
	term_shutdown();
	wm_destroy_window(wid);
	return rc < 0 ? -rc : rc;
}

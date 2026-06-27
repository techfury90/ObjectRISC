/*
 * mdview.c — the Ouroboros Markdown viewer (the north-star GUI app).
 *
 * Phase 2: read a real .md off /docs (hf_open/hf_read), parse a line-based
 * Markdown subset, and render it through the WM client font service with
 * PROPORTIONAL word-wrapping — the body is measured against the window width
 * via WM_OP_MEASURE_TEXT (the client can't measure proportional text itself).
 *
 *   # / ## / ###     headings        (bold luBS; H1 gets a hairline rule)
 *   - / *  item       bullet           (filled disc + wrapped proportional body)
 *   ``` ... ```        fenced code      (monospace lutRS on a tinted panel)
 *   blank line         paragraph gap
 *   anything else      paragraph        (wrapped proportional luRS)
 *
 * Shell/menu-spawned (edit/font_demo shape): wm_open_session + term_init +
 * hf_init, draw once, idle until q / Q / Esc.  A later phase scrolls when the
 * document outruns the window (it clips at the bottom for now).
 */

#include "liborisc.h"

#define CONTENT_W  640
#define CONTENT_H  384
#define MARGIN_X   8
#define TEXT_W     (CONTENT_W - 2 * MARGIN_X)   /* proportional wrap width */
#define BULLET_IND 18                           /* bullet text indent */
#define LH         18                           /* line height */

/* OPEN LOOK gray-group palette (VEC_PALETTE_HEX). */
#define COL_PAPER   11   /* White  #f5f5f5 — the page            */
#define COL_INK     14   /* Black  #000000 — text                */
#define COL_RULE    12   /* BG2    #b8b8b8 — rule + bullet disc   */
#define COL_CODEBG  10   /* BG1    #cccccc — code-block panel     */

#define DOC_PATH    "/docs/welcome.md"
#define DOC_MAX     8192

static char doc[DOC_MAX];
static int  doc_len;

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

/* Read `path` into doc[] (NUL-terminated).  hf_read needs a stack buffer, so we
 * read in chunks and copy into our own (writable) data buffer.  Returns 0, or a
 * negative error. */
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

/* Greedy word-wrap: draw `text` in `face` from (x,y), breaking at word
 * boundaries so each rendered line measures <= max_w (via wm_measure_text —
 * only the WM holds the proportional advances).  Returns the y past the last
 * line. */
static int
draw_wrapped(int face, const char *text, int x, int y, int max_w)
{
	char line[160], cand[160];
	int  ll = 0;
	const char *p = text;

	line[0] = 0;
	while (*p) {
		while (*p == ' ') p++;            /* skip run of spaces */
		if (!*p) break;
		const char *ws = p;               /* word start */
		while (*p && *p != ' ') p++;
		int wl = (int)(p - ws);

		/* candidate = line + (space if non-empty) + word */
		int cl = 0, i;
		for (i = 0; i < ll; i++) cand[cl++] = line[i];
		if (ll > 0 && cl < (int)sizeof(cand) - 1) cand[cl++] = ' ';
		for (i = 0; i < wl && cl < (int)sizeof(cand) - 1; i++) cand[cl++] = ws[i];
		cand[cl] = 0;

		int w = wm_measure_text(face, cand);
		if (w >= 0 && w <= max_w) {
			for (i = 0; i <= cl; i++) line[i] = cand[i];   /* accept */
			ll = cl;
		} else {
			if (ll > 0) { vec_text(face, x, y, line); y += LH; }   /* flush */
			ll = 0;
			for (i = 0; i < wl && ll < (int)sizeof(line) - 1; i++) line[ll++] = ws[i];
			line[ll] = 0;
		}
	}
	if (ll > 0) { vec_text(face, x, y, line); y += LH; }
	return y;
}

/* Parse + render doc[] top to bottom. */
static void
render_doc(void)
{
	vec_set_color(COL_PAPER);
	vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	int y = 6;
	int in_code = 0;
	const char *p = doc;

	while (*p && y < CONTENT_H) {
		const char *ls = p;
		while (*p && *p != '\n') p++;
		int len = (int)(p - ls);
		if (*p == '\n') p++;

		char ln[256];
		int i, n = len < (int)sizeof(ln) - 1 ? len : (int)sizeof(ln) - 1;
		for (i = 0; i < n; i++) ln[i] = ls[i];
		ln[n] = 0;

		/* ``` fence — toggle code mode, draw nothing for the fence itself. */
		if (ln[0] == '`' && ln[1] == '`' && ln[2] == '`') { in_code = !in_code; continue; }

		if (in_code) {
			vec_set_color(COL_CODEBG);
			vec_rect_fill(MARGIN_X, y - 2, TEXT_W, LH);
			vec_set_color(COL_INK);
			vec_text(FONT_FACE_MONO, MARGIN_X + 4, y, ln);
			y += LH;
			continue;
		}

		if (n == 0) { y += 8; continue; }   /* blank — paragraph gap */

		if (ln[0] == '#') {                  /* heading */
			int level = 0;
			while (ln[level] == '#') level++;
			const char *h = ln + level;
			while (*h == ' ') h++;
			vec_set_color(COL_INK);
			y = draw_wrapped(FONT_FACE_BOLD, h, MARGIN_X, y, TEXT_W);
			if (level == 1) {                /* H1 gets a hairline rule */
				vec_set_color(COL_RULE);
				vec_line(MARGIN_X, y, CONTENT_W - MARGIN_X - 1, y);
				y += 8;
			} else {
				y += 4;
			}
			continue;
		}

		if ((ln[0] == '-' || ln[0] == '*') && ln[1] == ' ') {   /* bullet */
			vec_set_color(COL_RULE);
			vec_oval_fill(MARGIN_X + 4, y + 5, 5, 5);
			vec_set_color(COL_INK);
			y = draw_wrapped(FONT_FACE_PROP, ln + 2, MARGIN_X + BULLET_IND, y,
			                 TEXT_W - BULLET_IND);
			continue;
		}

		vec_set_color(COL_INK);             /* paragraph */
		y = draw_wrapped(FONT_FACE_PROP, ln, MARGIN_X, y, TEXT_W);
	}
}

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("Markdown Viewer", &wid);
	if (rc != 0) {
		WP("mdview: wm_open_session failed\n");
		return rc;
	}

	term_init();

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { WP("mdview: bind VECTOR failed\n"); goto out; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { WP("mdview: vec_init failed\n");    goto out; }

	if (load_doc(DOC_PATH) != 0) {
		vec_set_color(COL_PAPER); vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);
		vec_set_color(COL_INK);
		vec_text(FONT_FACE_PROP, 8, 8, "mdview: could not open " DOC_PATH);
	} else {
		render_doc();
	}

	for (;;) {
		int code = 0, mods = 0;
		if (term_pollkey(&code, &mods) == 0 && should_quit(code))
			break;
		task_yield();
	}

out:
	term_shutdown();
	wm_destroy_window(wid);
	return rc < 0 ? -rc : rc;
}

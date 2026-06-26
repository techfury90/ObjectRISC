/*
 * font_demo.c — OPEN LOOK font specimen (desktop-menu launchable).
 *
 * Draws, AS A REAL CLIENT PROGRAM, samples of all three baked WM faces
 * through the client font service (VEC_OP_TEXT_* / vec_text):
 *
 *   - Lucida Sans        (FONT_FACE_PROP,  luRS)  — proportional headings
 *   - Lucida Typewriter  (FONT_FACE_MONO,  lutRS) — monospace body
 *   - OPEN LOOK glyphs    (FONT_FACE_GLYPH, olgl)  — pushpins / menu marks
 *
 * Until the font service landed those faces were WM-internal chrome; this is
 * the first program to draw proportional Lucida + OPEN LOOK glyphs into its
 * own window, and the same path the Markdown viewer will render through.
 *
 * Shell/menu-spawned (mouse_paint shape): wm_open_session for the window,
 * term_init so we can read the quit key, bind the vector surface, draw once,
 * then idle until q / Q / Esc (or the title-bar menu button closes us).
 */

#include "liborisc.h"

/* The window content area is a fixed 80x24 cell grid (8x16 px cells). */
#define CONTENT_W  640
#define CONTENT_H  384

/* OPEN LOOK palette indices (VEC_PALETTE_HEX / the olgx gray group). */
#define COL_PAPER  11   /* White #f5f5f5 — the page    */
#define COL_INK    14   /* Black #000000 — text        */
#define COL_LABEL  13   /* BG3   #666666 — captions     */
#define COL_RULE   12   /* BG2   #b8b8b8 — hairline rule */

/* pushpin-out, pushpin-in, default-pin-out, abbrev menu button + inverted,
 * solid vertical menu mark (olgl codepoints, olgx.h). */
static const unsigned char OLGL_ROW[] = { 19, 20, 21, 22, 23, 47 };

static int
should_quit(int code)
{
	return code == 'q' || code == 'Q' || code == TK_ESCAPE;
}

/* Draw the specimen.  The whole page is sent as one burst of vector ops — the
 * WM drains a batch per wake (WM_DRAIN_MAX) and vec_text packs ~8 glyphs per
 * SEND, so the ~50 ops stay well under the depth-64 queue.  No pacing needed
 * (the per-line task_yields are gone — they only forced extra WM wakes). */
static void
draw_specimen(void)
{
	vec_set_color(COL_PAPER); vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	vec_set_color(COL_INK);
	vec_text(FONT_FACE_PROP, 8, 6, "Object RISC -- OPEN LOOK Font Specimen");
	vec_set_color(COL_RULE);  vec_line(8, 26, CONTENT_W - 9, 26);

	vec_set_color(COL_LABEL);
	vec_text(FONT_FACE_PROP, 8, 38, "Lucida Sans (luRS, proportional):");
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_PROP, 16, 56,
	         "The quick brown fox jumps over the lazy dog 0123456789");

	vec_set_color(COL_LABEL);
	vec_text(FONT_FACE_PROP, 8, 86, "Lucida Typewriter (lutRS, monospace):");
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_MONO, 16, 104, "$ ls -la *.orx | grep ouroboros");

	vec_set_color(COL_LABEL);
	vec_text(FONT_FACE_PROP, 8, 134, "OPEN LOOK glyphs (olgl):");
	vec_set_color(COL_INK);
	for (int i = 0; i < (int)sizeof(OLGL_ROW); i++) {
		vec_text_move(FONT_FACE_GLYPH, 16 + i * 28, 152);
		vec_text_char(OLGL_ROW[i]);
	}

	vec_set_color(COL_LABEL);
	vec_text(FONT_FACE_PROP, 8, 182, "(press q or Esc to close)");
}

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("Font Specimen", &wid);
	if (rc != 0) {
		print_str("font_demo: wm_open_session failed: ");
		print_int(rc); print_str("\n");
		return rc;
	}

	/* Own keyboard mailbox (O9) + a real WM subscription, so term_pollkey
	 * reads OUR keys, not whatever the spawning shell parked. */
	term_init();

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { print_str("font_demo: bind VECTOR failed\n"); goto out; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { print_str("font_demo: vec_init failed\n");    goto out; }

	draw_specimen();

	/* Idle until the user dismisses us (q / Q / Esc, or the title-bar menu
	 * button). */
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

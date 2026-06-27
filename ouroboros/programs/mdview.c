/*
 * mdview.c — the Ouroboros Markdown viewer (the north-star GUI app).
 *
 * Phase 1: open a window, bind the VECTOR surface, and render a representative
 * Markdown page — H1 + rule, body paragraphs (proportional luRS), an H2, a
 * bullet list, and a monospace code block (lutRS on a tinted panel) — through
 * the WM client font service (vec_text / vec_rect / vec_line / vec_oval).  The
 * page is hardcoded for now; Phase 2 reads a real .md off hostfsd and parses
 * it, and a later phase drives the OPEN LOOK scrollbar from the scroll offset.
 *
 * Shell/menu-spawned (font_demo shape): wm_open_session for the window,
 * term_init for the quit key, bind the vector surface, draw once, idle until
 * q / Q / Esc (or the title-bar menu button closes us).
 */

#include "liborisc.h"

/* The window content area is a fixed 80x24 cell grid (8x16 px cells). */
#define CONTENT_W  640
#define CONTENT_H  384

/* OPEN LOOK gray-group palette (VEC_PALETTE_HEX). */
#define COL_PAPER   11   /* White  #f5f5f5 — the page             */
#define COL_INK     14   /* Black  #000000 — body + heading text  */
#define COL_LABEL   13   /* BG3    #666666 — muted footer          */
#define COL_RULE    12   /* BG2    #b8b8b8 — hairline rule + bullet */
#define COL_CODEBG  10   /* BG1    #cccccc — code-block panel       */

static int
should_quit(int code)
{
	return code == 'q' || code == 'Q' || code == TK_ESCAPE;
}

/* One bullet-list item: a small filled disc + proportional text. */
static void
bullet(int y, const char *s)
{
	vec_set_color(COL_RULE);  vec_oval_fill(12, y + 5, 5, 5);
	vec_set_color(COL_INK);   vec_text(FONT_FACE_PROP, 26, y, s);
}

/* Render the (hardcoded) Markdown page as one burst of vector ops. */
static void
draw_page(void)
{
	/* the paper */
	vec_set_color(COL_PAPER);
	vec_rect_fill(0, 0, CONTENT_W, CONTENT_H);

	/* # The Ouroboros Markdown Viewer */
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_BOLD, 8, 8, "The Ouroboros Markdown Viewer");
	vec_set_color(COL_RULE);
	vec_line(8, 30, CONTENT_W - 9, 30);

	/* body paragraph (proportional luRS) */
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_PROP, 8, 42,
	         "The first real application rendered on Object RISC --");
	vec_text(FONT_FACE_PROP, 8, 60,
	         "proportional Lucida Sans, drawn through the WM font service.");

	/* ## Features */
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_BOLD, 8, 92, "Features");

	bullet(116, "Bold headings over proportional body text");
	bullet(138, "Monospace code blocks on a tinted panel");
	bullet(160, "An OPEN LOOK scrollbar that will actually scroll");

	/* a fenced code block: tinted panel + monospace lutRS */
	vec_set_color(COL_CODEBG);
	vec_rect_fill(8, 190, CONTENT_W - 16, 40);
	vec_set_color(COL_INK);
	vec_text(FONT_FACE_MONO, 16, 196, "int fd = hf_open(\"README.md\", HF_O_RDONLY);");
	vec_text(FONT_FACE_MONO, 16, 212, "while ((n = hf_read(fd, buf, sizeof buf)) > 0)");

	/* footer */
	vec_set_color(COL_LABEL);
	vec_text(FONT_FACE_PROP, 8, 360, "(press q or Esc to close)");
}

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("Markdown Viewer", &wid);
	if (rc != 0) {
		print_str("mdview: wm_open_session failed: ");
		print_int(rc); print_str("\n");
		return rc;
	}

	/* Own keyboard mailbox (O9) + a real WM subscription so term_pollkey
	 * reads OUR keys, not whatever the spawning shell parked. */
	term_init();

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { print_str("mdview: bind VECTOR failed\n"); goto out; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { print_str("mdview: vec_init failed\n");    goto out; }

	draw_page();

	/* Idle until dismissed (q / Q / Esc, or the title-bar menu button). */
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

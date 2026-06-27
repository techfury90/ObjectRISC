/*
 * font_smoke.c — wire test for the WM client font service (VEC_OP_TEXT_*).
 *
 * Exercises vec_text_move / vec_text_char / vec_text across all three baked
 * faces — proportional Lucida Sans (luRS), monospace Lucida Typewriter
 * (lutRS), and the OPEN LOOK glyph face (olgl) — and verifies every SEND
 * dispatches (returns 0).  This proves the client -> WM wire path; pixel
 * accuracy is eyeballed via the interactive specimen (font_demo.orx, launched
 * from the desktop menu) on `make boot`.
 *
 * Direct-launch shape (mirrors vec_smoke.c): task_init + DIR_SLOT promote +
 * wm_init + wm_new_window + wm_bind_surface(VECTOR).  font_demo.orx, by
 * contrast, is shell/menu-spawned and uses wm_open_session.
 */

#include "liborisc.h"

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

#define WP(s) do { restore_or_state(); print_str(s); } while (0)

static void
fail(const char *stage, int got)
{
	restore_or_state();
	print_str("FAIL: "); print_str(stage);
	print_str(" got="); print_int(got); print_str("\n");
}

static void
promote_boot_parent_to_dir_slot(void)
{
	asm volatile("orefld o1, 544(o12)\norefst o1, 584(o12)" : : : "r1");
}

/* A few recognisable olgl codepoints: pushpin-out, pushpin-in, menu button. */
static const unsigned char OLGL_ROW[] = { 19, 20, 22 };

int
main(void)
{
	int rc, i;

	task_init();
	promote_boot_parent_to_dir_slot();
	WP("font_smoke: starting\n");

	rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }

	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0 || wid < 1) { fail("new_window", rc); return 2; }

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { fail("bind VECTOR", rc); return 3; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { fail("vec_init", rc); return 4; }

	if (vec_set_color(14)) { fail("set_color", 0); return 5; }
	if (vec_text(FONT_FACE_PROP, 8, 8,  "luRS proportional")) { fail("luRS", 0);  return 6; }
	if (vec_text(FONT_FACE_BOLD, 8, 18, "luBS bold"))         { fail("luBS", 0);  return 10; }
	if (vec_text(FONT_FACE_MONO, 8, 28, "lutRS mono"))        { fail("lutRS", 0); return 7; }
	for (i = 0; i < (int)sizeof(OLGL_ROW); i++)
		if (vec_text_move(FONT_FACE_GLYPH, 8 + i * 24, 48) ||
		    vec_text_char(OLGL_ROW[i])) { fail("olgl", i); return 8; }

	WP("font_smoke: text ops dispatched OK\n");

	/* WM_OP_MEASURE_TEXT — the client asks the WM for text widths (only it holds
	 * the proportional tables).  lutRS is 8px monospace, so "Hello" = 5*8 = 40
	 * exactly; the proportional faces are non-zero but variable; "" = 0. */
	int mw = wm_measure_text(FONT_FACE_MONO, "Hello");
	if (mw != 40)                                      { fail("measure mono", mw);  return 11; }
	if (wm_measure_text(FONT_FACE_PROP, "Hello") <= 0) { fail("measure prop", 0);   return 12; }
	if (wm_measure_text(FONT_FACE_MONO, "") != 0)      { fail("measure empty", 0);  return 13; }
	WP("font_smoke: measure_text OK\n");

	WP("font_smoke: PASS\n");

	rc = wm_destroy_window(wid);
	if (rc != 0) { fail("destroy_window", rc); return 9; }
	return 0;
}

/*
 * vec_smoke.c — smoke test for the WM-mediated vector graphics
 * service (Phase 59 / WM γ.11).
 *
 * Mirrors wm_smoke.c's shape: same boot-environment (--service args
 * for O3 / O4 / O8), same DIR_SLOT promotion, same wm_init flow.
 * Once the WM is reachable and a CONSOLE window is allocated, we
 * exercise every VEC_OP_* through the libc wrappers and verify each
 * returns 0.
 *
 * Acceptance: all SENDs land without errors and the program exits
 * with status 0.  We don't visually verify pixels — the test is
 * about the wire path and the WM-side dispatch, not the rasteriser
 * accuracy (which the eyeball check on `make boot` covers).
 *
 * Test sequence:
 *   1. task_init + DIR_SLOT promotion + wm_init.
 *   2. wm_new_window(WIN_TYPE_CONSOLE).
 *   3. wm_bind_surface(wid, WSURF_VECTOR) — verify cap non-null.
 *   4. vec_init_from_dir_result() — copy DIR_RESULT_SLOT into
 *      WM_VECTOR_CAP_SLOT (libc reads from there on each SEND).
 *   5. vec_set_color(2)  (red).
 *   6. vec_line(10, 10, 100, 100).
 *   7. vec_rect_fill(120, 10, 80, 50).
 *   8. vec_rect_outline(220, 10, 80, 50).
 *   9. vec_set_color(4)  (blue).
 *  10. vec_oval_fill(320, 10, 80, 50).
 *  11. vec_oval_outline(420, 10, 80, 50).
 *  12. vec_clear()       (no-op WM-side; just verifies the dispatch).
 *  13. wm_destroy_window(wid).
 */

#include "liborisc.h"

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

static void
fail(const char *stage, int got)
{
	restore_or_state();
	print_str("FAIL: ");
	print_str(stage);
	print_str(" got=");
	print_int(got);
	print_str("\n");
}

#define WP(s)      do { restore_or_state(); print_str(s); } while (0)
#define WP_INT(n)  do { restore_or_state(); print_int(n); } while (0)

static void
promote_boot_parent_to_dir_slot(void)
{
	asm volatile(
		"orefld o1, 544(o12)\n"
		"orefst o1, 584(o12)"
		:
		:
		: "r1"
	);
}

int
main(void)
{
	task_init();
	promote_boot_parent_to_dir_slot();

	WP("vec_smoke: starting\n");

	int rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }

	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0)  { fail("new_window CONSOLE", rc); return 2; }
	if (wid < 1)  { fail("new_window wid invalid", wid); return 2; }
	WP("vec_smoke: new_window OK (wid=");
	WP_INT(wid);
	WP(")\n");

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { fail("bind VECTOR", rc); return 3; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind VECTOR cap null", 0); return 3; }
	}
	WP("vec_smoke: bind VECTOR OK\n");

	rc = vec_init_from_dir_result();
	if (rc != 0) { fail("vec_init_from_dir_result", rc); return 4; }
	WP("vec_smoke: cap stashed in WM_VECTOR_CAP_SLOT\n");

	rc = vec_set_color(2);
	if (rc != 0) { fail("vec_set_color red", rc); return 5; }

	rc = vec_line(10, 10, 100, 100);
	if (rc != 0) { fail("vec_line", rc); return 6; }

	rc = vec_rect_fill(120, 10, 80, 50);
	if (rc != 0) { fail("vec_rect_fill", rc); return 7; }

	rc = vec_rect_outline(220, 10, 80, 50);
	if (rc != 0) { fail("vec_rect_outline", rc); return 8; }

	rc = vec_set_color(4);
	if (rc != 0) { fail("vec_set_color blue", rc); return 9; }

	rc = vec_oval_fill(320, 10, 80, 50);
	if (rc != 0) { fail("vec_oval_fill", rc); return 10; }

	rc = vec_oval_outline(420, 10, 80, 50);
	if (rc != 0) { fail("vec_oval_outline", rc); return 11; }

	rc = vec_clear();
	if (rc != 0) { fail("vec_clear", rc); return 12; }
	WP("vec_smoke: all ops dispatched OK\n");

	rc = wm_destroy_window(wid);
	if (rc != 0) { fail("destroy_window", rc); return 13; }
	WP("vec_smoke: destroy OK\n");

	WP("vec_smoke: PASS\n");
	return 0;
}

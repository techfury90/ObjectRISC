/*
 * raster_smoke.c — smoke test for WM-mediated raster blit
 * (Phase 59 / WM γ.12).
 *
 * Same shape as vec_smoke.c.  Acceptance: every libc helper
 * returns 0 and the program exits 0.  Pixel accuracy isn't
 * asserted — eyeball check on `make boot` covers that.
 *
 * Test sequence:
 *   1. task_init + DIR_SLOT promotion + wm_init.
 *   2. wm_new_window(WIN_TYPE_CONSOLE).
 *   3. wm_bind_surface(wid, WSURF_RASTER) — verify cap non-null.
 *   4. raster_init_from_dir_result() — copy DIR_RESULT_SLOT into
 *      WM_RASTER_CAP_SLOT.
 *   5. raster_blit() with a static palette-indexed 16×16 buffer
 *      drawn at (50, 50).  Static buffer sits in boot data, so
 *      the libc routes the SEND through O15.
 *   6. raster_blit() again with a stack-local 8×8 buffer drawn
 *      at (200, 50).  Stack-local lives in the boot stack range,
 *      so the libc routes through O11.
 *   7. raster_clear() — verifies the no-op CLEAR dispatch path.
 *   8. wm_destroy_window.
 */

#include "liborisc.h"

/* Static checkerboard — 16×16, alternating palette idx 2 (red) and
 * idx 4 (blue).  Lives in boot data; raster_blit picks O15 for it. */
static const unsigned char checker_16x16[16 * 16] = {
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
	2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,
	4,2,4,2,4,2,4,2,4,2,4,2,4,2,4,2,
};

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

	WP("raster_smoke: starting\n");

	int rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }

	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0) { fail("new_window", rc); return 2; }
	if (wid < 1) { fail("new_window wid invalid", wid); return 2; }
	WP("raster_smoke: new_window OK (wid=");
	WP_INT(wid);
	WP(")\n");

	rc = wm_bind_surface(wid, WSURF_RASTER);
	if (rc != 0) { fail("bind RASTER", rc); return 3; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind RASTER cap null", 0); return 3; }
	}
	WP("raster_smoke: bind RASTER OK\n");

	rc = raster_init_from_dir_result();
	if (rc != 0) { fail("raster_init_from_dir_result", rc); return 4; }

	rc = raster_blit(WM_PACK_XY(50, 50), WM_PACK_WH(16, 16), checker_16x16);
	if (rc != 0) { fail("raster_blit (data)", rc); return 5; }
	WP("raster_smoke: blit (data) OK\n");

	unsigned char stack_pixels[8 * 8];
	{
		int i;
		for (i = 0; i < 64; i++) stack_pixels[i] = (i & 1) ? 5 : 6;
	}
	rc = raster_blit(WM_PACK_XY(200, 50), WM_PACK_WH(8, 8), stack_pixels);
	if (rc != 0) { fail("raster_blit (stack)", rc); return 6; }
	WP("raster_smoke: blit (stack) OK\n");

	rc = raster_clear();
	if (rc != 0) { fail("raster_clear", rc); return 7; }
	WP("raster_smoke: clear OK\n");

	rc = wm_destroy_window(wid);
	if (rc != 0) { fail("destroy_window", rc); return 8; }
	WP("raster_smoke: destroy OK\n");

	WP("raster_smoke: PASS\n");
	return 0;
}

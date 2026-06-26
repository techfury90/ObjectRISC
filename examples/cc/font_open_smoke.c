/*
 * font_open_smoke.c — wire test for the WM font_open(name) client API.
 *
 * font_open asks the WM to load /fonts/<name>.wmf and return a face id usable
 * as the `face` arg to vec_text.  This exercises:
 *   - a BUILT-IN name ("luRS") resolving to its fixed id (FONT_FACE_PROP = 0),
 *   - a NEW name ("luBI", Lucida Bold Italic) loading into a fresh slot (id 4),
 *   - RE-OPEN of that name returning the same id (idempotent, no second load),
 *   - a MISSING name ("nofont") replying a negative error.
 *
 * Needs a WM with /fonts wired (oriscdir + hostfsd) — see the run harness.
 * Pixel accuracy is eyeballed on `make boot`; this proves the open/dispatch/
 * id-assignment path returns 0.
 */

#include "liborisc.h"

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

#define WP(s) do { restore_or_state(); print_str(s); } while (0)

static void
promote_boot_parent_to_dir_slot(void)
{
	asm volatile("orefld o1, 544(o12)\norefst o1, 584(o12)" : : : "r1");
}

int
main(void)
{
	int builtin, opened, reopened, missing;

	task_init();
	promote_boot_parent_to_dir_slot();
	WP("font_open_smoke: starting\n");

	builtin  = font_open("luRS");    /* built-in → FONT_FACE_PROP = 0 */
	opened   = font_open("luBI");    /* new      → first dynamic id = 4 */
	reopened = font_open("luBI");    /* re-open  → same id, no reload   */
	missing  = font_open("nofont");  /* absent   → negative error       */

	restore_or_state();
	print_str("font_open: luRS="); print_int(builtin);
	print_str(" luBI=");           print_int(opened);
	print_str(" luBI2=");          print_int(reopened);
	print_str(" nofont=");         print_int(missing);
	print_str("\n");

	if (builtin == FONT_FACE_PROP && opened == 4 && reopened == 4 && missing < 0)
		WP("font_open_smoke: PASS\n");
	else
		WP("font_open_smoke: FAIL\n");
	return 0;
}

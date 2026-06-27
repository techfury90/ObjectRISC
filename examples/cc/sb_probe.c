/* sb_probe.c — window-keeper for the scrollbar auto-repeat test.
 *
 * Creates a console window (so the WM paints a scrollbar on it) and then idles
 * forever, blocked in pointer_getevent.  The scrollbar test injects arrow-hold
 * pointer events that the WM consumes ITSELF (it never forwards a scrollbar hit
 * to the client), so this probe just keeps its window alive while the WM
 * auto-repeats.  The test kills it at the end and checks the WM's -DSB_DEBUG
 * "sb elev=" trace.  Mirrors ptr_smoke.c's setup; only the final loop differs.
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
	print_str("FAIL: "); print_str(stage); print_str(" got=");
	print_int(got); print_str("\n");
}

static void
promote_boot_parent_to_dir_slot(void)
{
	asm volatile("orefld o1, 544(o12)\norefst o1, 584(o12)" : : : "r1");
}

int
main(void)
{
	task_init();
	promote_boot_parent_to_dir_slot();
	WP("sb_probe: starting\n");

	int rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }

	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0) { fail("new_window", rc); return 2; }
	if (wid < 1) { fail("new_window wid invalid", wid); return 2; }

	rc = wm_bind_surface(wid, WSURF_POINTER);
	if (rc != 0) { fail("bind POINTER", rc); return 3; }

	rc = pointer_init_from_dir_result();
	if (rc != 0) { fail("pointer_init_from_dir_result", rc); return 4; }

	rc = pointer_subscribe();
	if (rc != 0) { fail("pointer_subscribe", rc); return 5; }
	WP("sb_probe: ready\n");

	/* Idle forever.  The WM swallows the injected scrollbar events, so this
	 * blocks on events that never arrive; the test tears us down. */
	for (;;) {
		int evt_type, packed_xy, button, btn_state;
		pointer_getevent(&evt_type, &packed_xy, &button, &btn_state);
	}
	return 0;
}

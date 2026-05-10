/*
 * ptr_smoke.c — smoke test for WM-mediated pointer events
 * (Phase 59 / WM γ.13).
 *
 * Subscribes through the WM-mediated pointer cap, then polls the
 * libc-allocated event mailbox (O10) for events the WM forwards
 * from oriscterm.  fake_terminal injects a fixed event sequence at
 * test launch (motion + click + drag + release); we count what
 * arrives and PASS when we see the expected number.
 *
 * Test sequence:
 *   1. task_init + DIR_SLOT promotion + wm_init.
 *   2. wm_new_window(WIN_TYPE_CONSOLE) — required to bind a
 *      surface even though the v1 pointer service is single-WM-wide.
 *   3. wm_bind_surface(wid, WSURF_POINTER) — verify cap non-null.
 *   4. pointer_init_from_dir_result() + pointer_subscribe().
 *   5. Poll-loop: until we've received MIN_EVENTS or hit
 *      MAX_ITERATIONS, drain pointer_getevent() and print one line
 *      per event.
 *   6. pointer_unsubscribe + wm_destroy_window.
 *
 * fake_terminal.py's --event flag injects events as soon as a
 * subscriber appears — we just need to be subscribed by the time
 * the events fire.  See test_ptr_smoke.sh for the exact sequence.
 */

#include "liborisc.h"

#define MIN_EVENTS     3
#define MAX_ITERATIONS 50000

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

	WP("ptr_smoke: starting\n");

	int rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }

	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0) { fail("new_window", rc); return 2; }
	if (wid < 1) { fail("new_window wid invalid", wid); return 2; }

	rc = wm_bind_surface(wid, WSURF_POINTER);
	if (rc != 0) { fail("bind POINTER", rc); return 3; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind POINTER cap null", 0); return 3; }
	}
	WP("ptr_smoke: bind POINTER OK\n");

	rc = pointer_init_from_dir_result();
	if (rc != 0) { fail("pointer_init_from_dir_result", rc); return 4; }

	rc = pointer_subscribe();
	if (rc != 0) { fail("pointer_subscribe", rc); return 5; }
	WP("ptr_smoke: subscribed\n");

	int seen = 0;
	int it;
	for (it = 0; it < MAX_ITERATIONS && seen < MIN_EVENTS; it++) {
		int evt_type, packed_xy, button, btn_state;
		rc = pointer_getevent(&evt_type, &packed_xy,
		                      &button, &btn_state);
		if (rc == 0) {
			seen++;
			WP("ptr_smoke: evt type=");
			WP_INT(evt_type);
			WP(" xy=");
			WP_INT(packed_xy);
			WP(" btn=");
			WP_INT(button);
			WP(" mask=");
			WP_INT(btn_state);
			WP("\n");
		}
	}

	if (seen < MIN_EVENTS) {
		fail("expected >=3 events", seen);
		return 6;
	}

	pointer_unsubscribe();
	rc = wm_destroy_window(wid);
	if (rc != 0) { fail("destroy_window", rc); return 7; }

	WP("ptr_smoke: PASS (seen=");
	WP_INT(seen);
	WP(")\n");
	return 0;
}

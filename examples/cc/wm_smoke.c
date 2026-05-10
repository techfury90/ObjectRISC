/*
 * wm_smoke.c — smoke test for the oriscwm window-manager wire
 * protocol, exercised through the libc wm.c wrappers.
 *
 * Milestone-3 cleanup: replaces the inline-asm SEND-and-poll
 * dance with calls to wm_init / wm_new_window / wm_bind_surface /
 * wm_destroy_window.  The smoke test now looks like a normal
 * client program — task_init, dir-bootstrap, then high-level WM
 * calls.  Discovers the WM at /sys/wm/0 via dir_walk (libc's
 * wm_init does this) instead of wiring its idx directly via
 * --service.
 *
 * Boot environment (set up by test_wm_smoke.sh's --service args):
 *
 *     O3 = our data segment   (preserved by task_init in O15)
 *     O4 = our self-service
 *     O8 = oriscdir mailbox sub-cap (BOOT_PARENT_SLOT after
 *          task_init; we promote it to DIR_SLOT so dir.c finds
 *          the directory without going through SUP_OP_GET_DIR —
 *          we have no parent supervisor)
 *
 * Test sequence (same shape as milestone 2; just simpler code):
 *
 *     1. task_init + promote BOOT_PARENT to DIR_SLOT.
 *     2. wm_init() — walk /sys/wm/0, populate WM_SLOT.
 *     3. wm_new_window(WIN_TYPE_CONSOLE) — verify status, wid,
 *        geometry.
 *     4. wm_bind_surface(wid, WSURF_CONSOLE)  — succeeds.
 *     5. wm_bind_surface(wid, WSURF_KEYBOARD) — succeeds.
 *     6. wm_bind_surface(wid, WSURF_GRID)     — WIN_E_INVAL.
 *     7. wm_new_window(WIN_TYPE_CONSOLE) again — WIN_E_NOSPC.
 *     8. wm_new_window(WIN_TYPE_GRAPHICAL)    — WIN_E_NOTIMPL.
 *     9. wm_destroy_window(wid) — ok.
 *    10. wm_new_window(WIN_TYPE_CONSOLE) — slot reuse OK.
 *
 * Owner-task ref convention: wm_new_window expects the owner ref
 * in O1.  We pass null (`onull o1`) because this single-task
 * program has no convenient way to construct a self-referential
 * task ref — the WM's task_query auto-destroy returns a non-zero
 * status on null and skips the window, which is fine for the
 * smoke test (we manually destroy at step 9 + step 10's slot-reuse
 * implicitly confirms no leak).
 */

#include "liborisc.h"

/* After every wire op (dir_walk inside wm_init, plus every wm_*
 * SEND-and-poll), the receive-queue overlay has filled O2..O4 from
 * the reply's or_payload — clobbering the boot stack ref (O2) and
 * data ref (O3) that print_int / print_str depend on.  task_init
 * parks the boot stack in O11 and the boot data in O15, so we
 * restore from there before any diagnostic.  Same idiom as the
 * supervisor's sup_restore_boot_or. */
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

/* Promote BOOT_PARENT_SLOT (=boot O8 = oriscdir cap, parked here
 * by task_init) into DIR_SLOT so dir.c finds the directory without
 * going through SUP_OP_GET_DIR.  Same dance the supervisor does at
 * boot — see ouroboros/supervisor.c.
 *
 * Hardcoded offsets BOOT_PARENT_SLOT_OFFSET = 544, DIR_SLOT_OFFSET
 * = 584 (per task.c's slot map). */
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

	WP("wm_smoke: starting\n");

	/* Step 1: walk /sys/wm/0 and populate WM_SLOT. */
	int rc = wm_init();
	if (rc != 0) { fail("wm_init", rc); return 1; }
	WP("wm_smoke: wm_init OK\n");

	/* Steps 2-3: allocate a CONSOLE window.  Pass null owner ref
	 * (see comment at file top — single-task programs can't
	 * construct a self-task ref easily). */
	int wid = 0, w_cells = 0, h_cells = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0)         { fail("new_window CONSOLE #1", rc); return 2; }
	if (wid < 1)         { fail("new_window wid invalid", wid); return 2; }
	if (w_cells == 0)    { fail("new_window w_cells zero", w_cells); return 2; }
	if (h_cells == 0)    { fail("new_window h_cells zero", h_cells); return 2; }
	WP("wm_smoke: new_window CONSOLE OK (wid=");
	WP_INT(wid);
	WP(", w_cells=");
	WP_INT(w_cells);
	WP(", h_cells=");
	WP_INT(h_cells);
	WP(")\n");

	/* Steps 4-5: bind console + keyboard surfaces.  wm_bind_surface
	 * parks the resolved cap into DIR_RESULT_SLOT (offset 616) — we
	 * verify it by oisn'ing what landed there. */
	rc = wm_bind_surface(wid, WSURF_CONSOLE);
	if (rc != 0) { fail("bind CONSOLE", rc); return 4; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind CONSOLE cap null", 0); return 4; }
	}
	rc = wm_bind_surface(wid, WSURF_KEYBOARD);
	if (rc != 0) { fail("bind KEYBOARD", rc); return 5; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind KEYBOARD cap null", 0); return 5; }
	}
	WP("wm_smoke: bind CONSOLE + KEYBOARD OK\n");

	/* Step 6: bind GRID on a CONSOLE window — Phase 59 / WM γ.9 made
	 * this succeed, returning an R|S sub-cap of the per-window GRID
	 * service.  Verify the resolved cap is non-null. */
	rc = wm_bind_surface(wid, WSURF_GRID);
	if (rc != 0) { fail("bind GRID", rc); return 6; }
	{
		int isn;
		asm volatile("orefld o1, 616(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		if (isn) { fail("bind GRID cap null", 0); return 6; }
	}
	WP("wm_smoke: bind GRID OK\n");

	/* Step 6b (Phase 60 step 5): wm_get_geometry on this window
	 * should return the same dimensions as wm_new_window did, plus
	 * non-zero pixel extents.  Probe with wid=0 to also verify the
	 * "first live window" default. */
	{
		int geom[4];
		geom[0] = 0; geom[1] = 0; geom[2] = 0; geom[3] = 0;
		rc = wm_get_geometry(0, geom);
		if (rc != 0)                          { fail("get_geometry rc", rc); return 6; }
		if (geom[WM_GEOM_W_CELLS] != w_cells) { fail("get_geometry w_cells", geom[WM_GEOM_W_CELLS]); return 6; }
		if (geom[WM_GEOM_H_CELLS] != h_cells) { fail("get_geometry h_cells", geom[WM_GEOM_H_CELLS]); return 6; }
		if (geom[WM_GEOM_W_PX] == 0)          { fail("get_geometry w_px zero", geom[WM_GEOM_W_PX]); return 6; }
		if (geom[WM_GEOM_H_PX] == 0)          { fail("get_geometry h_px zero", geom[WM_GEOM_H_PX]); return 6; }
		WP("wm_smoke: get_geometry OK (");
		WP_INT(geom[WM_GEOM_W_CELLS]); WP("x"); WP_INT(geom[WM_GEOM_H_CELLS]);
		WP(" cells / ");
		WP_INT(geom[WM_GEOM_W_PX]);    WP("x"); WP_INT(geom[WM_GEOM_H_PX]);
		WP(" px)\n");
	}

	/* Step 6c (Phase 60 step 8): wm_set_title round-trip on the live
	 * window.  We don't verify the rendered output here (no FB
	 * snapshot path in the smoke harness yet) — just that the SEND
	 * returns success.  Visual correctness gets checked on boot.sh. */
	rc = wm_set_title(wid, "wm_smoke");
	if (rc != 0) { fail("set_title", rc); return 6; }
	WP("wm_smoke: set_title OK\n");

	/* Step 7 (Phase 60 step 11): second CONSOLE allocation — N=1
	 * restriction is gone; expect a fresh wid != the first.  Both
	 * windows live on the screen with cascade-positioned z-stacking. */
	int wid_b = 0, dummy_w = 0, dummy_h = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid_b, &dummy_w, &dummy_h);
	if (rc != 0)        { fail("new_window CONSOLE #2", rc); return 7; }
	if (wid_b == wid)   { fail("new_window CONSOLE #2 wid collision", wid_b); return 7; }
	if (wid_b < 1)      { fail("new_window CONSOLE #2 wid invalid", wid_b); return 7; }
	WP("wm_smoke: second CONSOLE OK (wid=");
	WP_INT(wid_b);
	WP(")\n");

	/* Set a distinct title on the second window. */
	rc = wm_set_title(wid_b, "secondary");
	if (rc != 0) { fail("set_title #2", rc); return 7; }

	/* Tear down the second window before the rest of the test
	 * proceeds — keeps the slot-reuse check at step 10 deterministic. */
	rc = wm_destroy_window(wid_b);
	if (rc != 0) { fail("destroy_window #2", rc); return 7; }
	int dummy_wid = 0;

	/* Step 8: GRAPHICAL — must fail WIN_E_NOTIMPL. */
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_GRAPHICAL, &dummy_wid, &dummy_w, &dummy_h);
	if (rc != WIN_E_NOTIMPL) {
		fail("new_window GRAPHICAL expected WIN_E_NOTIMPL", rc); return 8;
	}
	WP("wm_smoke: GRAPHICAL not-implemented (expected)\n");

	/* Step 9: destroy. */
	rc = wm_destroy_window(wid);
	if (rc != 0) { fail("destroy_window", rc); return 9; }
	WP("wm_smoke: destroy OK\n");

	/* Step 10: re-allocate to confirm slot reuse. */
	int wid2 = 0;
	asm volatile("onull o1");
	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid2, &dummy_w, &dummy_h);
	if (rc != 0)  { fail("new_window CONSOLE #3", rc); return 10; }
	if (wid2 < 1) { fail("new_window #3 wid invalid", wid2); return 10; }
	WP("wm_smoke: slot-reuse OK (wid=");
	WP_INT(wid2);
	WP(")\n");

	WP("wm_smoke: PASS\n");
	return 0;
}

/*
 * winhello.c — multi-window demo.  Opens its own WM-mediated window
 * (separate from the spawning shell's), prints a greeting into it,
 * waits a short while so the user can see the new window appear,
 * and tears it down on exit.
 *
 * Demonstrates Phase 60 step 12's `wm_open_session` helper:
 * everything term_print / grid_write does between the helper
 * returning and wm_destroy_window lands in the new window.
 * print_str (firmware ConsoleWrite) still goes to the simorisc
 * stdout regardless — useful for diagnostics that survive even
 * when the window itself doesn't render correctly.
 */

#include "liborisc.h"

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("winhello", &wid);
	if (rc != 0) {
		print_str("winhello: wm_open_session failed: ");
		print_int(rc);
		print_str("\n");
		return rc;
	}

	/* term_print_only_init parks the boot O2/O3/O4 into the slots
	 * term.c's _term_console_write expects.  Skips the keyboard
	 * subscribe that term_init would do — we don't want to steal
	 * focus from the parent shell's keyboard subscription. */
	term_print_only_init();
	term_print("Hello from winhello!\n");
	term_print("This window will close in a moment.\n");

	/* Yield a few times so the WM finishes compositing the title
	 * bar + greeting before we tear the window down.  No proper
	 * sleep primitive yet — a tight task_yield loop is the lightest
	 * "give other tasks a slice" mechanism we have.  Count is
	 * tuned for a roughly half-second wait at simorisc's
	 * interpreted rate; bump if the new window flickers in and out
	 * before the user can see it. */
	int i;
	for (i = 0; i < 10000; i++) task_yield();

	wm_destroy_window(wid);

	print_str("winhello: done\n");
	return 0;
}

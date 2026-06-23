/*
 * menudemo.c — exercises the libc menu_run helper.
 *
 * Opens its own WM window, binds + subscribes the pointer so the
 * menu is mouse-driven (it also works with the arrow keys + Enter),
 * then loops popping a menu and reporting the choice.  "Quit" or
 * cancel (Esc / click-off) ends the program.
 *
 * Demonstrates the reusable menu framework a program gets for free:
 * one menu_run call, a flat NUL-separated item buffer, an int back.
 */

#include "liborisc.h"

/* Flat item buffer: 5 NUL-terminated strings end to end.  Index 4
 * ("Quit") ends the loop. */
static const char demo_items[] =
	"Say Hello\0"
	"Count to 3\0"
	"Cheer\0"
	"Clear\0"
	"Quit";
#define DEMO_N 5

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("menu demo", &wid);
	if (rc != 0) {
		print_str("menudemo: wm_open_session failed: ");
		print_int(rc);
		print_str("\n");
		return rc;
	}

	term_init();

	/* Bind + subscribe the pointer so the menu is mouse-driven.
	 * menu_run degrades to keyboard-only if this fails, so treat
	 * pointer setup as best-effort. */
	if (wm_bind_surface(wid, WSURF_POINTER) == 0
	    && pointer_init_from_dir_result() == 0) {
		pointer_subscribe();
	}

	term_print("menu demo — right side is a pop-up menu.\n");
	term_print("Mouse: hover + click.  Keys: up/down + Enter, Esc.\n");

	int quit = 0;
	while (!quit) {
		/* Pop the menu at cell (2, 4).  Returns the chosen index
		 * or -1 on cancel. */
		int pick = menu_run(2, 4, demo_items, DEMO_N);

		/* The menu drew over rows 4..8; wipe the canvas so each
		 * round starts clean (the demo has nothing else on the
		 * grid layer to preserve). */
		grid_clear();

		if (pick < 0) {
			term_print("(cancelled)\n");
			quit = 1;
		} else if (pick == 0) {
			term_print("Hello!\n");
		} else if (pick == 1) {
			term_print("1... 2... 3!\n");
		} else if (pick == 2) {
			term_print("Hooray!\n");
		} else if (pick == 3) {
			term_clear();
		} else if (pick == 4) {
			term_print("bye!\n");
			quit = 1;
		}
	}

	if (pointer_subscribed()) pointer_unsubscribe();
	term_shutdown();
	wm_destroy_window(wid);
	return 0;
}

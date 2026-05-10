/*
 * winhello.c — multi-window demo with input focus.
 *
 * Opens its own WM-mediated window (separate from the spawning
 * shell's), subscribes to keyboard so keystrokes flow to *us*
 * instead of the shell, reads a line of text, echoes it back, then
 * tears the window down on exit.  The shell reclaims keyboard focus
 * via term_resubscribe() in its cmd_run foreground path.
 *
 * Demonstrates Phase 60 step 13's focus-handoff dance:
 *   1. wm_open_session installs the new window's CONSOLE / KEYBOARD
 *      / GRID caps into O5 / O6 / O7.
 *   2. term_init allocates our private mailbox + SENDs subscribe to
 *      O6 (= the WM keyboard service), making us the WM's single
 *      keyboard subscriber — keystrokes now arrive at *our* queue.
 *   3. read_line echoes characters back into our window via O5.
 *   4. term_shutdown unsubscribes; wm_destroy_window tears down the
 *      window.  Parent shell's task_wait returns, then its own
 *      term_resubscribe re-emits the shell-side subscribe so the
 *      next prompt sees keystrokes again.
 */

#include "liborisc.h"

#define WINHELLO_INPUT_MAX 80

static int
winhello_read_line(char *buf, int max)
{
	int n = 0;
	for (;;) {
		int mods = 0;
		int c = term_getkey(&mods);
		if (c < 0) continue;
		/* TK_RETURN is the host display worker's Tk-side keycode for
		 * the Enter key — 0x10D, distinct from ASCII '\r' / '\n'.  Tk
		 * delivers keysyms verbatim through the input-sink queue; the
		 * libc liborisc.h defines TK_RETURN / TK_BACKSPACE / TK_TAB /
		 * TK_ESCAPE / arrow keys etc. for clients to check against. */
		if (c == TK_RETURN || c == '\r' || c == '\n') {
			term_print_char('\n');
			break;
		}
		if (c == TK_BACKSPACE || c == 0x08) {
			if (n > 0) {
				n--;
				term_print_char('\b');
			}
			continue;
		}
		if (n < max - 1 && c >= 32 && c < 127) {
			buf[n++] = (char)c;
			term_print_char((char)c);
		}
	}
	buf[n] = '\0';
	return n;
}

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

	/* Full term_init (not just term_print_only_init): allocates the
	 * private mailbox + queue and SUBSCRIBES to the keyboard via our
	 * freshly-installed O6.  After this, keystrokes route to us
	 * instead of the shell. */
	term_init();

	term_print("Hello from winhello!\n");
	term_print("Type a line and press Enter: ");

	char line[WINHELLO_INPUT_MAX];
	winhello_read_line(line, WINHELLO_INPUT_MAX);

	term_print("You typed: ");
	term_print(line);
	term_print("\n");

	/* Give the user a beat to read the echo. */
	int i;
	for (i = 0; i < 4000; i++) task_yield();

	/* Release our subscription before destroying the window so the
	 * WM doesn't keep routing events to a dead mailbox. */
	term_shutdown();
	wm_destroy_window(wid);

	print_str("winhello: done\n");
	return 0;
}

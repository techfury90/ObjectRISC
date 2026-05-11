/*
 * mouse_paint.c — pointer-driven painting demo, WM edition.
 *
 * The pre-Ouroboros version of this program (examples/cc/mouse_paint.c
 * pre-Phase-60) talked to oriscterm's vector / pointer services
 * directly with boot-time --service slots.  This rewrite is a regular
 * shell-launchable program: it opens its own WM window, binds the
 * window's vector and pointer surfaces, and draws inside the window.
 *
 *   left button:   click drops a small filled square; drag draws a
 *                  stroke between successive motion samples while the
 *                  button is held.
 *   middle button: cycles the pen color (palette 1..8).
 *   right button:  clears the canvas.
 *   q / Q / ESC:   exit cleanly (releases keyboard / pointer, destroys
 *                  the window; shell reclaims focus).
 *
 * The WM (oriscwm) translates pointer events to the topmost window's
 * content-area-local coordinates before forwarding, so the pointer
 * coords we get here line up 1:1 with the vec_* coordinate space.
 */

#include "liborisc.h"

#define MIN_COLOR  1
#define MAX_COLOR  8

/* Non-blocking keyboard poll.  term_getkey() uses an infinite
 * timeout (it's designed for line-input flows like winhello);
 * mouse_paint runs an event loop that has to interleave pointer
 * and keyboard polls, so we peek the keyboard mailbox (O9, set up
 * by term_init) with timeout=0 instead. */
static int
term_pollkey(int *out_code, int *out_mods)
{
	int status, code, mods;
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 0\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0"
		: "=r"(status), "=r"(code), "=r"(mods)
		:
		: "r1", "r2", "r3", "r4"
	);
	/* Restore the libc-stashed boot stack / data refs that the
	 * ReceiveQueuePoll clobbered.  Same dance term.c's blocking
	 * term_getkey performs. */
	asm volatile("omov o2, o11\nomov o3, o15");
	if (status != 0) return -1;
	if (out_code) *out_code = code;
	if (out_mods) *out_mods = mods;
	return 0;
}

static int
should_quit(int code)
{
	return code == 'q' || code == 'Q' || code == TK_ESCAPE;
}

int
main(void)
{
	task_init();

	int wid = 0;
	int rc = wm_open_session("mouse_paint", &wid);
	if (rc != 0) {
		print_str("mouse_paint: wm_open_session failed: ");
		print_int(rc);
		print_str("\n");
		return rc;
	}

	rc = wm_bind_surface(wid, WSURF_VECTOR);
	if (rc != 0) { print_str("mouse_paint: bind VECTOR failed\n"); goto out_destroy; }
	rc = vec_init_from_dir_result();
	if (rc != 0) { print_str("mouse_paint: vec_init failed\n");    goto out_destroy; }

	rc = wm_bind_surface(wid, WSURF_POINTER);
	if (rc != 0) { print_str("mouse_paint: bind POINTER failed\n"); goto out_destroy; }
	rc = pointer_init_from_dir_result();
	if (rc != 0) { print_str("mouse_paint: pointer_init failed\n"); goto out_destroy; }
	rc = pointer_subscribe();
	if (rc != 0) { print_str("mouse_paint: pointer_subscribe failed\n"); goto out_unsub; }

	int color = 1;
	vec_set_color(color);
	vec_clear();

	term_print("mouse_paint — left=draw  middle=color  right=clear  "
	           "q/ESC=quit\n");

	int last_x = -1, last_y = -1;
	int quit = 0;
	while (!quit) {
		int code = 0;
		int mods = 0;
		/* pcc-orisc emits `la r, 0` for null-pointer literals in
		 * arg position, which the assembler rejects — always pass
		 * a real address. */
		if (term_pollkey(&code, &mods) == 0 && should_quit(code)) {
			quit = 1;
			break;
		}

		int evt_type, packed_xy, button, btn_state;
		if (pointer_getevent(&evt_type, &packed_xy,
		                     &button, &btn_state) != 0) {
			task_yield();
			continue;
		}

		int x = (packed_xy >> 16) & 0xFFFF;
		int y = packed_xy & 0xFFFF;

		if (evt_type == PTR_EVT_DOWN) {
			if (button == PTR_BTN_LEFT) {
				vec_rect_fill(x - 2, y - 2, 4, 4);
				last_x = x; last_y = y;
			} else if (button == PTR_BTN_MIDDLE) {
				color = (color % MAX_COLOR) + 1;
				vec_set_color(color);
				term_print("color ");
				/* term_print only takes strings; emit digit as
				 * a one-char buffer. */
				char buf[2];
				buf[0] = '0' + color;
				buf[1] = '\0';
				term_print(buf);
				term_print("\n");
			} else if (button == PTR_BTN_RIGHT) {
				vec_clear();
				vec_set_color(color);
				term_print("cleared\n");
			}
		} else if (evt_type == PTR_EVT_MOTION) {
			/* Stroke only while left is held; without the gate
			 * every hover would smear paint everywhere. */
			if ((btn_state & (1 << PTR_BTN_LEFT)) && last_x >= 0) {
				vec_line(last_x, last_y, x, y);
				last_x = x; last_y = y;
			}
		} else if (evt_type == PTR_EVT_UP) {
			if (button == PTR_BTN_LEFT) {
				last_x = -1; last_y = -1;
			}
		}
	}

	term_print("mouse_paint: bye\n");
	pointer_unsubscribe();
out_unsub:
	term_shutdown();
out_destroy:
	wm_destroy_window(wid);
	return rc < 0 ? -rc : rc;
}

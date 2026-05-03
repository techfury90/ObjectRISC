/*
 * mouse_paint.c — pointer-driven painting demo for oriscterm.
 *
 * Subscribes to the terminal's pointer service (idx 6) and draws on
 * the canvas in response to mouse events:
 *
 *     left button:   click drops a small filled square; drag draws
 *                    a stroke (line segments between successive
 *                    motion samples while the button is held).
 *     middle button: clicks cycle the pen colour (palette 1..8).
 *     right button:  click clears the canvas.
 *
 * Quit: close the Tk terminal window (oriscrun tears the rest down).
 *
 * Boot environment expected (set up by the runner):
 *     O3 = our data segment   (preserved as O15 — see "OR hygiene")
 *     O4 = our self-service   (queue receives pointer events here)
 *     O8 = terminal vector    (idx 4) — drawing primitives
 *     O9 = terminal pointer   (idx 6) — subscription target
 *
 * OR hygiene
 * ----------
 * Same pattern as kbd_echo. Queue dispatch overlays O1..O4 from the
 * wire AND any SEND clobbers them too — vec_cmd issues SEND so it
 * has to clean up. We park the boot O2/O3/O4 into O13/O14/O15 once
 * at startup, and EVERY helper that issues a SEND or a primitive
 * call restores them on its way out. That way the loop body never
 * has to think about OR state — print_str / print_int "just work"
 * after any helper returns.
 */

#include "liborisc.h"

#define PTR_MOTION 0x00
#define PTR_DOWN   0x01
#define PTR_UP     0x02

#define PTR_BTN_LEFT   1
#define PTR_BTN_MIDDLE 2
#define PTR_BTN_RIGHT  3

#define VEC_LINE          0x00
#define VEC_RECT_FILL     0x01
#define VEC_CLEAR         0x05
#define VEC_SET_COLOR     0x06

/* Restore the three OR slots libc cares about (stack/data refs) plus
 * the queue target. Called at the bottom of every helper that issues
 * a primitive or a SEND. */
static void
restore_or_state(void)
{
	asm volatile("omov o2, o13");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

static int
attach_self_queue_16(void)
{
	int status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 16\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	restore_or_state();
	return status;
}

/* Subscribe to the pointer service (in O9). Derives an R|S self-ref
 * into a scratch OR (o7) and SENDs it. */
static void
ptr_subscribe(void)
{
	register void *__or o7_subref __asm__("o7");
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 9\n"        /* R|S */
		"call  #0x103\n"
		"nop\n"
		"omov  %0, o1"
		: "=r"(o7_subref)
		:
		: "r1", "r2", "r4"
	);
	asm volatile(
		"omov  o1, o9\n"
		"omov  o2, o7\n"
		"onull o3\n"
		"addiu r4, r0, 0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		:
		: "r1", "r4", "r5", "r6", "r7"
	);
	restore_or_state();
}

/* Block until a pointer event arrives. Stores type, packed xy,
 * button, and current button-state mask via out-pointers. */
static int
ptr_poll(int *out_type, int *out_xy, int *out_button, int *out_state)
{
	int status, t, xy, button, state;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0\n"
		"addu  %4, r6, r0"
		: "=r"(status), "=r"(t), "=r"(xy), "=r"(button), "=r"(state)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	*out_type   = t;
	*out_xy     = xy;
	*out_button = button;
	*out_state  = state;
	restore_or_state();
	return status;
}

static void
vec_cmd(int cmd, int a, int b)
{
	asm volatile(
		"omov  o1, o8\n"
		"onull o2\n"
		"onull o3\n"
		"addu  r4, %0, r0\n"
		"addu  r5, %1, r0\n"
		"addu  r6, %2, r0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "r"(cmd), "r"(a), "r"(b)
		: "r1", "r4", "r5", "r6", "r7"
	);
	restore_or_state();
}

static int
pack16(int hi, int lo)
{
	return ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
}

int
main(void)
{
	register void *__or o2_stack       __asm__("o2");
	register void *__or o3_data        __asm__("o3");
	register void *__or o4_self        __asm__("o4");
	register void *__or o13_stack_save __asm__("o13");
	register void *__or o14_self_save  __asm__("o14");
	register void *__or o15_data_save  __asm__("o15");

	int status, type, xy, button, state;
	int x, y;
	int last_x = -1, last_y = -1;
	int color = 1;

	o13_stack_save = o2_stack;
	o14_self_save  = o4_self;
	o15_data_save  = o3_data;

	if (attach_self_queue_16() != 0) {
		print_str("attach failed\n");
		return 1;
	}

	ptr_subscribe();

	vec_cmd(VEC_CLEAR, 0, 0);
	vec_cmd(VEC_SET_COLOR, color, 0);

	print_str("mouse_paint ready — left=draw  middle=color  "
	          "right=clear  (close the window to quit)\n");

	while (1) {
		if (ptr_poll(&type, &xy, &button, &state) != 0) break;

		x = (xy >> 16) & 0xFFFF;
		y = xy & 0xFFFF;

		if (type == PTR_DOWN) {
			if (button == PTR_BTN_LEFT) {
				vec_cmd(VEC_RECT_FILL,
				        pack16(x - 2, y - 2), pack16(4, 4));
				last_x = x; last_y = y;
			} else if (button == PTR_BTN_MIDDLE) {
				color = (color % 8) + 1;
				vec_cmd(VEC_SET_COLOR, color, 0);
				print_str("color cycled to ");
				print_int(color);
				print_str("\n");
			} else if (button == PTR_BTN_RIGHT) {
				vec_cmd(VEC_CLEAR, 0, 0);
				vec_cmd(VEC_SET_COLOR, color, 0);
				print_str("canvas cleared\n");
			}
		} else if (type == PTR_MOTION) {
			/* Draw only while the left button is held — that's
			 * a "drag stroke." Without the gate every stray
			 * mouse hover would smear paint everywhere. */
			if ((state & (1 << PTR_BTN_LEFT)) && last_x >= 0) {
				vec_cmd(VEC_LINE,
				        pack16(last_x, last_y), pack16(x, y));
				last_x = x; last_y = y;
			}
		} else if (type == PTR_UP) {
			if (button == PTR_BTN_LEFT) {
				last_x = -1; last_y = -1;
			}
		}
	}

	return 0;
}

/*
 * paint.c — interactive painting demo for oriscterm's multi-capability
 * service objects. Wires up keyboard (idx 2) for input and vector
 * (idx 4) for actual drawing.
 *
 * Boot environment expected (set up by the runner):
 *     O3 = our data segment   (preserved as O15 — see "OR hygiene")
 *     O4 = our self-service   (full caps; queue receives key events here)
 *     O6 = terminal keyboard  (idx 2) — subscription target
 *     O8 = terminal vector    (idx 4) — drawing primitives
 *
 * Controls:
 *     Arrows  — move the (logical) cursor by 8 pixels
 *     D       — draw a small filled square at the cursor
 *     L       — draw a line from the previous D-position to the cursor
 *     R       — draw a rectangle outline 16×8 at the cursor
 *     O       — draw an oval     outline 16×8 at the cursor
 *     C       — cycle pen colour (palette indices 1..8)
 *     Space   — clear the canvas
 *     ESC     — exit
 *
 * The cursor itself doesn't render visibly — its position is shown
 * in lines printed to host stdout (look for [cpu0] in the runner
 * output). A future revision can render it on the canvas via grid
 * once the protocol is settled.
 */

#include "liborisc.h"

/* See tools/devices/oriscterm — keep these synced. */
#define KEY_ESCAPE    0x11B
#define KEY_UP        0x180
#define KEY_DOWN      0x181
#define KEY_LEFT      0x182
#define KEY_RIGHT     0x183

#define VEC_LINE          0x00
#define VEC_RECT_FILL     0x01
#define VEC_RECT_OUTLINE  0x02
#define VEC_OVAL_OUTLINE  0x04
#define VEC_CLEAR         0x05
#define VEC_SET_COLOR     0x06

#define STEP 8        /* pixels per arrow keypress */

/* --- helpers in their own functions so main's register pressure
 *     stays manageable. Each one does the inline-asm and restores O3
 *     on the way out so callers can use print_str freely. --- */

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
	return status;
}

/* Subscribe to the keyboard service — pass keyboard ref in O6 (already
 * there from the boot ABI). Derives an R|S self-ref into a scratch OR
 * and SENDs it. Return 0 on success. */
static int
kbd_subscribe(void)
{
	register void *__or o9_subref __asm__("o9");
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 9\n"        /* R|S */
		"call  #0x103\n"
		"nop\n"
		"omov  %0, o1"
		: "=r"(o9_subref)
		:
		: "r1", "r2", "r4"
	);
	asm volatile(
		"omov  o1, o6\n"
		"omov  o2, o9\n"
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
	return 0;
}

/* Block until a key event arrives. Stores codepoint and modifiers
 * via out-pointers and returns the primitive's status. */
static int
kbd_poll(int *out_code, int *out_mods)
{
	int status, code, mods;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0"
		: "=r"(status), "=r"(code), "=r"(mods)
		:
		: "r1", "r2", "r3", "r4"
	);
	*out_code = code;
	*out_mods = mods;
	return status;
}

/* SEND a vector command to the terminal's vector service (in O8).
 * R4 = cmd, R5 = a, R6 = b. R7 unused. Vector commands never need
 * a payload OR — null O2/O3 for safety. */
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
}

/* SEND a grid-write command to the terminal's grid service (in O7).
 * The source buffer ref is our data segment, parked in O15 — restore
 * it into O2 for the wire payload. R4=offset, R5=len, R6=col, R7=row. */
static void
grid_text(int offset, int length, int col, int row)
{
	asm volatile(
		"omov  o1, o7\n"
		"omov  o2, o15\n"
		"onull o3\n"
		"addu  r4, %0, r0\n"
		"addu  r5, %1, r0\n"
		"addu  r6, %2, r0\n"
		"addu  r7, %3, r0\n"
		"send  o1"
		:
		: "r"(offset), "r"(length), "r"(col), "r"(row)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

static int
pack16(int hi, int lo)
{
	return ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
}

/* Restore O3 to the saved data ref (parked in O15 by main). Most asm
 * blocks here clobber O3 — call this before any print_str. */
static void
restore_o3(void)
{
	asm volatile("omov o3, o15");
}

const char title_str[] = "PAINT — Object RISC vector demo";

int
main(void)
{
	register void *__or o3_data        __asm__("o3");
	register void *__or o15_data_save  __asm__("o15");
	int code, mods;
	int x = 320, y = 200;
	int last_x = 320, last_y = 200;
	int color = 1;
	int title_off, title_len;

	o15_data_save = o3_data;

	if (attach_self_queue_16() != 0) {
		restore_o3();
		print_str("attach failed\n");
		return 1;
	}
	restore_o3();

	kbd_subscribe();
	restore_o3();

	vec_cmd(VEC_CLEAR,     0, 0);
	vec_cmd(VEC_SET_COLOR, color, 0);
	restore_o3();

	/* Render a title via the grid service. The data ref's storage
	 * starts at VA 0x40000 (CONTRACT.md §2); subtract that to get
	 * the byte offset of title_str[] within the object. */
	title_off = (int)title_str - 0x40000;
	title_len = (int)strlen(title_str);
	grid_text(title_off, title_len, 2, 0);
	restore_o3();

	print_str("paint demo running — focus the terminal\n");

	while (1) {
		if (kbd_poll(&code, &mods) != 0) break;
		restore_o3();
		if (code == KEY_ESCAPE) break;

		if (code == KEY_UP)         { y -= STEP; if (y < 0) y = 0; }
		else if (code == KEY_DOWN)  { y += STEP; }
		else if (code == KEY_LEFT)  { x -= STEP; if (x < 0) x = 0; }
		else if (code == KEY_RIGHT) { x += STEP; }
		else if (code == 'd' || code == 'D') {
			vec_cmd(VEC_RECT_FILL,
			        pack16(x - 2, y - 2), pack16(4, 4));
			last_x = x; last_y = y;
		}
		else if (code == 'l' || code == 'L') {
			vec_cmd(VEC_LINE,
			        pack16(last_x, last_y), pack16(x, y));
			last_x = x; last_y = y;
		}
		else if (code == 'r' || code == 'R') {
			vec_cmd(VEC_RECT_OUTLINE,
			        pack16(x - 8, y - 4), pack16(16, 8));
		}
		else if (code == 'o' || code == 'O') {
			vec_cmd(VEC_OVAL_OUTLINE,
			        pack16(x - 8, y - 4), pack16(16, 8));
		}
		else if (code == 'c' || code == 'C') {
			color = (color % 8) + 1;
			vec_cmd(VEC_SET_COLOR, color, 0);
		}
		else if (code == ' ') {
			vec_cmd(VEC_CLEAR, 0, 0);
			vec_cmd(VEC_SET_COLOR, color, 0);
		}
		restore_o3();

		print_str("cursor (");
		print_int(x);
		print_str(", ");
		print_int(y);
		print_str(")  pen=");
		print_int(color);
		print_str("\n");
	}

	restore_o3();
	print_str("paint demo exiting\n");
	return 0;
}

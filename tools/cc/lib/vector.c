/*
 * vector.c — WM-mediated vector graphics client (Phase 59 / WM γ.11).
 *
 * Each vec_*() helper builds a SEND with int_payload[0..2] = (op,
 * packed1, packed2), addressed to the per-task VECTOR cap stashed
 * in WM_VECTOR_CAP_SLOT.  The WM polls its per-window VECTOR queue,
 * dispatches on op, and rasterises the result into the framebuffer.
 *
 * The cap-slot indirection (rather than a fixed boot OPR like O7
 * for grid) means leader→child propagation can be plumbed later
 * without touching the libc.  For now the smoke test seeds the
 * slot from DIR_RESULT_SLOT after a successful wm_bind_surface
 * (WSURF_VECTOR) — supervisor-level wiring is a follow-up.
 *
 * All helpers return 0 on success, -1 if WM_VECTOR_CAP_SLOT is null
 * (no surface bound).  The SEND itself is fire-and-forget; the WM
 * polls and rasterises asynchronously.
 *
 * Boot OPR restoration: SEND clobbers O2..O4 with a reply-queue
 * overlay.  We restore O2 = boot stack (O11) and O3 = boot data
 * (O15) after every SEND — same idiom as term.c / grid.c.
 *
 * Wire op codes match oriscterm's VEC_* and oriscwm's
 * forward_vector_write dispatch.
 */

#include "liborisc.h"

/* Slot offsets — must match tools/cc/lib/task.c.  Hardcoded the
 * same way wm_smoke.c hardcodes BOOT_PARENT_SLOT / DIR_SLOT. */
#define DIR_RESULT_SLOT_OFFSET    616
#define WM_VECTOR_CAP_SLOT_OFFSET 712

/* Restore the boot OPRs the SEND clobbered. */
static void
_vec_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

/* Pack two 16-bit values into one 32-bit word — used for (x, y)
 * and (w, h) pairs.  Sign-extending halves keep negative
 * coordinates representable; the WM does its own clipping. */
static int
_vec_pack(int hi, int lo)
{
	return ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
}

/* Common SEND.  Loads WM_VECTOR_CAP_SLOT into O1; returns -1 if
 * null, 0 otherwise.  Caller has already loaded R4/R5/R6 via
 * input operands.  No source ref — vector ops carry their entire
 * payload in int_payload. */
static int
_vec_send(int op, int packed1, int packed2)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn  %0, o1"
		: "=r"(isn)
		: "i"(WM_VECTOR_CAP_SLOT_OFFSET)
		: "r1"
	);
	if (isn) return -1;
	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addu   r6, %3, r0\n"
		"send   o1"
		:
		: "i"(WM_VECTOR_CAP_SLOT_OFFSET),
		  "r"(op), "r"(packed1), "r"(packed2)
		: "r1", "r4", "r5", "r6"
	);
	_vec_restore_or();
	return 0;
}

/* Public: copy DIR_RESULT_SLOT (where wm_bind_surface lands its
 * resolved cap) into WM_VECTOR_CAP_SLOT.  Call once after a
 * successful wm_bind_surface(wid, WSURF_VECTOR).  Returns 0 if the
 * source slot was non-null, -1 otherwise. */
int
vec_init_from_dir_result(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1\n"
		"orefst o1, %2(o12)"
		: "=r"(isn)
		: "i"(DIR_RESULT_SLOT_OFFSET),
		  "i"(WM_VECTOR_CAP_SLOT_OFFSET)
		: "r1"
	);
	return isn ? -1 : 0;
}

int
vec_line(int x1, int y1, int x2, int y2)
{
	return _vec_send(VEC_OP_LINE,
	                 _vec_pack(x1, y1),
	                 _vec_pack(x2, y2));
}

int
vec_rect_fill(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_RECT_FILL,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_rect_outline(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_RECT_OUTLINE,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_oval_fill(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_OVAL_FILL,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_oval_outline(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_OVAL_OUTLINE,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_clear(void)
{
	return _vec_send(VEC_OP_CLEAR, 0, 0);
}

int
vec_set_color(int palette_idx)
{
	return _vec_send(VEC_OP_SET_COLOR, palette_idx, 0);
}

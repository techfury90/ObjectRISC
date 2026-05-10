/*
 * raster.c — WM-mediated raster blit client (Phase 59 / WM γ.12).
 *
 * Same shape as vector.c: each helper SENDs to the per-task RASTER
 * cap stashed in WM_RASTER_CAP_SLOT.  The wire payload is op +
 * packed (x, y) + packed (w, h) + byte offset into a source ref.
 *
 * Unlike vec_*(), raster_blit also has to pass a source ref in O2
 * for the WM to ObjFetchBytes from.  The caller's pixel buffer can
 * live in the boot data segment (for static / global arrays) or
 * the boot stack (for stack-locals); we pick the right boot OPR
 * by VA, same trick grid_print_n uses.
 */

#include "liborisc.h"

/* Slot offsets — must match tools/cc/lib/task.c. */
#define DIR_RESULT_SLOT_OFFSET    616
#define WM_RASTER_CAP_SLOT_OFFSET 976

/* VA layout — same as grid.c. */
#define DATA_VA       0x00040000U
#define STACK_BOTTOM  0x001f0000U

static void
_raster_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

/* SEND with source = boot stack (O11) — for stack-local pixel
 * buffers.  R4..R7 = (op, packed_xy, packed_wh, byte_offset). */
static void
_raster_send_stack(int op, int packed_xy, int packed_wh, int byte_off)
{
	asm volatile(
		"orefld o1, %0(o12)\n"
		"omov   o2, o11\n"
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addu   r6, %3, r0\n"
		"addu   r7, %4, r0\n"
		"send   o1"
		:
		: "i"(WM_RASTER_CAP_SLOT_OFFSET),
		  "r"(op), "r"(packed_xy), "r"(packed_wh), "r"(byte_off)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_raster_restore_or();
}

/* Same SEND, but source = boot data (O15) — for static / global
 * pixel buffers. */
static void
_raster_send_data(int op, int packed_xy, int packed_wh, int byte_off)
{
	asm volatile(
		"orefld o1, %0(o12)\n"
		"omov   o2, o15\n"
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addu   r6, %3, r0\n"
		"addu   r7, %4, r0\n"
		"send   o1"
		:
		: "i"(WM_RASTER_CAP_SLOT_OFFSET),
		  "r"(op), "r"(packed_xy), "r"(packed_wh), "r"(byte_off)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_raster_restore_or();
}

/* SEND with no source ref (CLEAR / future ops that don't carry
 * pixels). */
static void
_raster_send_nosrc(int op, int packed_xy, int packed_wh, int byte_off)
{
	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addu   r6, %3, r0\n"
		"addu   r7, %4, r0\n"
		"send   o1"
		:
		: "i"(WM_RASTER_CAP_SLOT_OFFSET),
		  "r"(op), "r"(packed_xy), "r"(packed_wh), "r"(byte_off)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_raster_restore_or();
}

/* OISN check — returns 1 if WM_RASTER_CAP_SLOT is null. */
static int
_raster_cap_isn(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		: "i"(WM_RASTER_CAP_SLOT_OFFSET)
		: "r1"
	);
	return isn;
}

int
raster_init_from_dir_result(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1\n"
		"orefst o1, %2(o12)"
		: "=r"(isn)
		: "i"(DIR_RESULT_SLOT_OFFSET),
		  "i"(WM_RASTER_CAP_SLOT_OFFSET)
		: "r1"
	);
	return isn ? -1 : 0;
}

int
raster_blit(int packed_xy, int packed_wh, const unsigned char *pixels)
{
	if (_raster_cap_isn()) return -1;

	unsigned int va = (unsigned int)pixels;
	if (va >= STACK_BOTTOM) {
		int byte_off = (int)(va - STACK_BOTTOM);
		_raster_send_stack(RST_OP_BLIT, packed_xy, packed_wh, byte_off);
	} else {
		int byte_off = (int)(va - DATA_VA);
		_raster_send_data(RST_OP_BLIT, packed_xy, packed_wh, byte_off);
	}
	return 0;
}

int
raster_clear(void)
{
	if (_raster_cap_isn()) return -1;
	_raster_send_nosrc(RST_OP_CLEAR, 0, 0, 0);
	return 0;
}

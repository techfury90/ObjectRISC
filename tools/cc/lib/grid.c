/*
 * grid.c — oriscterm grid (idx 3) client.
 *
 * The grid service paints byte sequences at integer (col, row)
 * positions on oriscterm's graphics canvas (currently 80×24).
 * Unlike the console service (idx 1), there's no implicit cursor
 * and no scrolling — caller picks the cell. The terminal pulls
 * the bytes via OBJ_READ_REQ once it sees the SEND, same async
 * dance as `term_print`.
 *
 * Clear-all is folded into the same service via a sentinel
 * (col=row=-1) on the SEND, which oriscterm honours by wiping
 * the whole canvas. Co-locating paint and clear on one ref keeps
 * the boot ABI to a single grid slot — the obvious alternative
 * (a separate vector-service ref) collides with hf_init's claim
 * on O8.
 *
 * Boot ABI (the shell's run_shell.sh and the libc `term_init`
 * cooperatively park these):
 *
 *     O5  = oriscterm console  (idx 1)
 *     O6  = oriscterm keyboard (idx 2)
 *     O7  = oriscterm grid     (idx 3)  — paint AND clear
 *     O11 = boot stack ref     (parked by term_init / term_print_only_init)
 *     O15 = boot data ref      (parked by term_init / term_print_only_init)
 *
 * `term_init` (or `term_print_only_init`) MUST have run before any
 * grid_* call — they do the O11/O15 parking. We don't redo it here
 * because doing so would clobber the receive-queue subscription
 * the shell-level term_init established.
 */

#include "liborisc.h"

/* Mirrors term.c's address-space constants — same VAs the boot
 * data and stack objects are mapped at. */
#define DATA_VA       0x00040000U
#define STACK_BOTTOM  0x001f0000U

/* Restore the boot OPRs the SEND clobbered. Mirrors term.c — same
 * O11/O14/O15 parking. */
static void
_grid_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

/* SEND a grid-write request, source = boot stack (O11). R4/R5 =
 * offset/length within the source; R6/R7 = destination (col, row).
 * pcc's Object RISC backend only passes four args in registers, so
 * we keep the helper at exactly four to avoid a stack-spill the
 * backend doesn't yet support. */
static void
_grid_write_stack(int offset, int count, int col, int row)
{
	asm volatile(
		"omov  o1, o7\n"
		"omov  o2, o11\n"
		"onull o3\n"
		"addu  r7, %3, r0\n"
		"addu  r6, %2, r0\n"
		"addu  r5, %1, r0\n"
		"addu  r4, %0, r0\n"
		"send  o1"
		:
		: "r"(offset), "r"(count), "r"(col), "r"(row)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_grid_restore_or();
}

/* Same SEND, but source = boot data (O15). */
static void
_grid_write_data(int offset, int count, int col, int row)
{
	asm volatile(
		"omov  o1, o7\n"
		"omov  o2, o15\n"
		"onull o3\n"
		"addu  r7, %3, r0\n"
		"addu  r6, %2, r0\n"
		"addu  r5, %1, r0\n"
		"addu  r4, %0, r0\n"
		"send  o1"
		:
		: "r"(offset), "r"(count), "r"(col), "r"(row)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_grid_restore_or();
}

/* grid_print_n — SEND `count` bytes from `buf` to grid at (col,
 * row). The caller picks the source object via the buf VA: stack
 * range routes through O11, otherwise through O15. */
void
grid_print_n(int col, int row, const char *buf, int count)
{
	unsigned int va = (unsigned int)buf;
	if (count <= 0) return;
	if (va >= STACK_BOTTOM) {
		_grid_write_stack((int)(va - STACK_BOTTOM), count, col, row);
	} else {
		_grid_write_data((int)(va - DATA_VA), count, col, row);
	}
}

/* grid_print — same as grid_print_n but lengths via strlen. */
void
grid_print(int col, int row, const char *s)
{
	int len = (int)strlen(s);
	grid_print_n(col, row, s, len);
}

/* grid_clear — wipe the entire canvas. Encoded as a grid-service
 * SEND (O7) with col=row=-1, which oriscterm recognises as the
 * clear-all sentinel; the source / offset / length are unused.
 *
 * We send via grid (O7) rather than vector because hf_init parks
 * its private mailbox into O8 — claiming a separate vector slot
 * would conflict. Treating "clear" as a grid command keeps the
 * full-screen-app boot ABI to a single service ref. */
void
grid_clear(void)
{
	asm volatile(
		"omov  o1, o7\n"
		"onull o2\n"
		"onull o3\n"
		"addiu r4, r0, 0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, -1\n"         /* col = 0xFFFFFFFF sentinel */
		"addiu r7, r0, -1\n"         /* row = 0xFFFFFFFF sentinel */
		"send  o1"
		:
		:
		: "r1", "r4", "r5", "r6", "r7"
	);
	_grid_restore_or();
}

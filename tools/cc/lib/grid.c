/*
 * grid.c — oriscterm grid (idx 3) client.
 *
 * The grid service paints byte sequences at integer (col, row)
 * positions on oriscterm's graphics canvas (currently 80×24).
 * Unlike the console service (idx 1), there's no implicit cursor
 * and no scrolling — caller picks the cell. The terminal pulls
 * the bytes via OBJ_READ_REQ once it sees the SEND, same async
 * dance as `term_print` (so the source buffer must outlive the
 * fetch — see the async-buffer note on grid_print_n below).
 *
 * Clear-all is folded into the same service via a sentinel
 * (col=row=-1) on the SEND, which oriscterm honours by wiping
 * the whole canvas.
 *
 * Phase 4: migrated onto the handle object API (obj.h). The grid
 * service cap — boot register O7 in liborisc's boot map — is adopted
 * into an `obj_t` handle the first time a grid_* helper runs
 * (obj_adopt_o7), and every paint/clear goes through obj_send_bytes
 * (fire-and-forget: no reply mailbox). All the raw omov/onull/send
 * inline asm is gone; only the boot-OR restore stays.
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
 * `task_init` (for the O12 handle table) and `term_init` (or
 * `term_print_only_init`, for the O11/O15 parking) MUST have run
 * before any grid_* call — the universal boot order every grid-using
 * program already follows (shell, login, edit, menudemo).
 */

#include "liborisc.h"
#include "obj.h"

/* Mirrors term.c's address-space constants — same VAs the boot
 * data and stack objects are mapped at. */
#define DATA_VA       0x00040000U
#define STACK_BOTTOM  0x001f0000U

/* The grid service (adopted from boot register O7), as a handle. */
static obj_t grid_svc_h = OBJ_NULL;

/* Restore the boot OPRs the SEND clobbered. Mirrors term.c — same
 * O11/O14/O15 parking. */
static void
_grid_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

/* Lazily adopt the O7 grid cap into a handle on first use (no caller-
 * facing init — keeps the grid_* API unchanged). Returns 0 once the
 * handle is live, -1 if the table isn't up (task_init not run) or O7
 * is null. */
static int
_grid_ensure(void)
{
	if (grid_svc_h >= 0)
		return 0;
	if (obj_init() != 0)
		return -1;
	grid_svc_h = obj_adopt_o7();
	return (grid_svc_h < 0) ? -1 : 0;
}

/* grid_print_n — SEND `count` bytes from `buf` to grid at (col, row).
 * The caller picks the source segment by the buf VA: stack range routes
 * through O11, otherwise through O15.
 *
 * The SEND is fire-and-forget — the terminal ObjFetchBytes the bytes
 * only after this returns — so `buf` must stay live and unchanged until
 * that async fetch. Callers in a render loop (cmd_view, edit) keep the
 * source buffer on a frame that outlives the loop; a transient callee
 * local would be overwritten before the fetch and render garbage. */
void
grid_print_n(int col, int row, const char *buf, int count)
{
	unsigned int va = (unsigned int)buf;
	int byte_off, src;

	if (count <= 0)
		return;
	if (_grid_ensure() != 0)
		return;
	if (va >= STACK_BOTTOM) {
		byte_off = (int)(va - STACK_BOTTOM);
		src = OBJ_SRC_STACK;
	} else {
		byte_off = (int)(va - DATA_VA);
		src = OBJ_SRC_DATA;
	}
	/* R4..R7 = (offset, count, col, row) — matches oriscterm's
	 * grid-write dispatch. */
	obj_send_bytes(grid_svc_h, src, OBJ_NULL, byte_off, count, col, row);
	_grid_restore_or();
}

/* grid_print — same as grid_print_n but lengths via strlen. */
void
grid_print(int col, int row, const char *s)
{
	int len = (int)strlen(s);
	grid_print_n(col, row, s, len);
}

/* grid_clear — wipe the entire canvas. Encoded as a grid-service SEND
 * with col=row=-1, which oriscterm recognises as the clear-all
 * sentinel; the source / offset / length are unused (no byte data). */
void
grid_clear(void)
{
	if (_grid_ensure() != 0)
		return;
	obj_send_bytes(grid_svc_h, OBJ_SRC_NONE, OBJ_NULL, 0, 0, -1, -1);
	_grid_restore_or();
}

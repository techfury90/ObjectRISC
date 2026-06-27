/*
 * pointer.c — WM-mediated pointer client (Phase 59 / WM γ.13).
 *
 * Phase 4: migrated onto the handle-based object API (obj.h). The
 * pointer-event mailbox is now an `obj_t` handle in the O12 handle table
 * (it used to be a hand-managed capability parked in O10, the last free
 * boot OPR — now reclaimed), the WM-mediated pointer service is adopted
 * straight from the dir-walk result into a handle, and subscribe /
 * unsubscribe / poll go through obj_send_or / obj_poll. All the raw
 * ObjAlloc / ReceiveQueueAttach / ObjDerive / send / ReceiveQueuePoll
 * inline asm is gone.
 *
 * Boot OPR convention still honoured here:
 *   O11 = boot stack ref, O15 = boot data ref. The object API uses
 *   O2/O3 as scratch (obj_send_or puts the subscriber sub-cap in O2),
 *   so we restore O2/O3 from O11/O15 after any SEND/poll — otherwise a
 *   following print_str would read string data through a clobbered O2.
 */

#include "liborisc.h"
#include "obj.h"

/* The WM-mediated pointer service (adopted from the dir-walk result)
 * and our own pointer-event mailbox, both as object handles. */
static obj_t wm_ptr_h   = OBJ_NULL;
static obj_t ptr_mbox_h = OBJ_NULL;

/* Mailbox caps mirror term.c's keyboard mailbox: R|W|S|V|C (== 0x5b).
 * The C bit is the derive-rights bit ObjDerive needs on the source. */
#define PTR_MBOX_CAPS \
	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

static void
_ptr_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

int
pointer_init_from_dir_result(void)
{
	if (obj_init() != 0)
		return -1;
	/* Adopt dir_walk's resolved WM-pointer ref into a handle (replaces
	 * the old "copy DIR_RESULT slot -> WM_POINTER_CAP slot" dance). */
	wm_ptr_h = obj_adopt_dir_result();
	return (wm_ptr_h < 0) ? -1 : 0;
}

int
pointer_subscribe(void)
{
	obj_t sub;

	if (wm_ptr_h < 0)
		return -1;

	/* Allocate our event mailbox and give it a receive queue. */
	ptr_mbox_h = obj_alloc(16, OBJ_TAG_SERVICE, PTR_MBOX_CAPS);
	if (ptr_mbox_h < 0)
		return -1;
	if (obj_queue_attach(ptr_mbox_h, 16) != 0)
		return -1;

	/* Derive an R|S sub-cap of the mailbox and SEND it to the WM as our
	 * subscriber ref (O2). R4..R7 = 0 (subscribe-all v1). The WM stashes
	 * its own copy, so we drop ours. */
	sub = obj_derive(ptr_mbox_h, OBJ_CAP_R | OBJ_CAP_S);
	if (sub < 0)
		return -1;
	obj_send_or(wm_ptr_h, sub, 0, 0, 0, 0);
	obj_drop(sub);
	_ptr_restore_or();
	return 0;
}

/* True (1) if pointer_subscribe has run and our event mailbox is live,
 * 0 otherwise. menu_run uses this to decide whether it can poll the
 * mouse — a program that never subscribed gets a keyboard-only menu. */
int
pointer_subscribed(void)
{
	return !obj_isnull(ptr_mbox_h);
}

int
pointer_unsubscribe(void)
{
	if (wm_ptr_h < 0)
		return -1;
	/* SEND with O2 = null clears the WM's subscriber slot — the same
	 * coarse v1 unsubscribe-all contract oriscterm uses. */
	obj_send_or(wm_ptr_h, OBJ_NULL, 0, 0, 0, 0);
	_ptr_restore_or();
	return 0;
}

int
pointer_getevent(int *evt_type, int *packed_xy,
                 int *button, int *btn_state)
{
	int out[4];

	if (obj_poll(ptr_mbox_h, out) != 0) {
		_ptr_restore_or();
		return -1;
	}
	*evt_type  = out[0];
	*packed_xy = out[1];
	*button    = out[2];
	*btn_state = out[3];
	_ptr_restore_or();
	return 0;
}

/* Expose the pointer-event mailbox handle for obj_waitset_* (block on pointer
 * events alongside other sources).  < 0 until pointer_subscribe has run. */
obj_t
pointer_event_mbox(void)
{
	return ptr_mbox_h;
}

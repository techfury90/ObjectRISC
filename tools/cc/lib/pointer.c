/*
 * pointer.c — WM-mediated pointer client (Phase 59 / WM γ.13).
 *
 * Allocates its own pointer-event mailbox (TAG_SERVICE) into O10,
 * attaches a receive queue, derives an R|S sub-cap, and SENDs it
 * as O2 to the WM-mediated pointer cap.  The WM stashes that ref
 * as its single subscriber and forwards every pointer event from
 * the underlying terminal (motion / down / up) to it.
 *
 * Boot OPR conventions used here:
 *   O10 = our pointer-event mailbox (allocated by pointer_subscribe)
 *   O11 = boot stack ref
 *   O15 = boot data ref
 *
 * O10 is the only previously-free OPR in liborisc's boot map (O9 is
 * reserved by term_init for the keyboard mailbox; O5/O6/O7/O14/O15
 * are reserved for surfaces and boot refs).
 */

#include "liborisc.h"

#define DIR_RESULT_SLOT_OFFSET     616
#define WM_POINTER_CAP_SLOT_OFFSET 1112

/* Tag + cap constants, mirrored from dir.c (each libc .c file
 * copies these locally — there's no shared firmware-constants
 * header yet). */
#define TAG_SERVICE  0x4103
#define CAP_R        0x01
#define CAP_W        0x02
#define CAP_S        0x08
#define CAP_V        0x04
#define CAP_C        0x10

static void
_ptr_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

static int
_ptr_cap_isn(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		: "i"(WM_POINTER_CAP_SLOT_OFFSET)
		: "r1"
	);
	return isn;
}

int
pointer_init_from_dir_result(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1\n"
		"orefst o1, %2(o12)"
		: "=r"(isn)
		: "i"(DIR_RESULT_SLOT_OFFSET),
		  "i"(WM_POINTER_CAP_SLOT_OFFSET)
		: "r1"
	);
	return isn ? -1 : 0;
}

int
pointer_subscribe(void)
{
	if (_ptr_cap_isn()) return -1;

	/* ObjAlloc(TAG_SERVICE, 0x5b) → O1; park into O10.
	 *
	 * Caps mirror term.c's keyboard-mailbox alloc — 0x5b includes
	 * a derive-rights bit at 0x40 that ObjDerive otherwise rejects
	 * the source for (status 3 = E_PERM-ish).  The exact bit name
	 * isn't in the firmware doc but the value is load-bearing. */
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"           /* TAG_SERVICE */
		"addiu r6, r0, 0x5b\n"
		"call  #0x100\n"               /* ObjAlloc */
		"nop\n"
		"omov  o10, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* ReceiveQueueAttach(depth=16). */
	asm volatile(
		"omov  o1, o10\n"
		"addiu r4, r0, 16\n"
		"call  #0x203\n"               /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	if (status != 0) return status;

	/* Derive R|S sub-cap of O10 → O2; SEND to WM_POINTER_CAP_SLOT
	 * with O2 = sub-cap, R4..R7 = zero (subscribe-all v1). */
	asm volatile(
		"omov   o1, o10\n"
		"addiu  r4, r0, %1\n"          /* R|S */
		"call   #0x103\n"              /* ObjDerive → O1 */
		"nop\n"
		"omov   o2, o1\n"
		"orefld o1, %0(o12)\n"
		"onull  o3\n"
		"addiu  r4, r0, 0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1"
		:
		: "i"(WM_POINTER_CAP_SLOT_OFFSET),
		  "i"(CAP_R | CAP_S)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	_ptr_restore_or();
	return 0;
}

/* True (1) if pointer_subscribe has run and our event mailbox (O10)
 * is live, 0 otherwise.  menu_run uses this to decide whether it can
 * poll the mouse — a program that never subscribed gets a keyboard-
 * only menu instead of a poll on a null mailbox. */
int
pointer_subscribed(void)
{
	int isn;
	asm volatile("oisn %0, o10" : "=r"(isn));
	return !isn;
}

int
pointer_unsubscribe(void)
{
	if (_ptr_cap_isn()) return -1;
	/* SEND with O2 = null clears the WM's subscriber slot.  Same
	 * coarse v1 contract oriscterm uses for keyboard / pointer
	 * unsubscribe-all. */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addiu  r4, r0, 0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1"
		:
		: "i"(WM_POINTER_CAP_SLOT_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);
	_ptr_restore_or();
	return 0;
}

/* See poll_window_grids in oriscwm — same status-via-global trick to
 * fit a 5-output capture (status + int_payload[0..3]) into pcc's
 * 4-output operand limit.  R2 (status) is sw'd to this global from
 * inside the asm body; R3..R6 use the regular outputs. */
static int _ptr_getevent_status;

int
pointer_getevent(int *evt_type, int *packed_xy,
                 int *button, int *btn_state)
{
	int et, pxy, btn, mask;
	asm volatile(
		"omov  o1, o10\n"
		"addiu r4, r0, 0\n"            /* timeout = 0 */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"la    r1, _ptr_getevent_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(et), "=r"(pxy), "=r"(btn), "=r"(mask)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	if (_ptr_getevent_status != 0) {
		_ptr_restore_or();
		return -1;
	}
	*evt_type  = et;
	*packed_xy = pxy;
	*button    = btn;
	*btn_state = mask;
	_ptr_restore_or();
	return 0;
}

/*
 * wm_smoke.c — smoke test for the oriscwm window-manager wire
 * protocol.  Exercises every op the milestone-1 daemon supports and
 * reports PASS/FAIL via firmware ConsoleWrite (host stdout, NOT the
 * Tk terminal).
 *
 * Test sequence:
 *
 *     1. Allocate self-mailbox queue + derive a R+S sub-cap (in O11)
 *        for use as reply_cap on every SEND.
 *     2. Register two surfaces with the WM:
 *           OP_REGISTER_SURFACE(WSURF_CONSOLE,  cap = O5)
 *           OP_REGISTER_SURFACE(WSURF_KEYBOARD, cap = O6)
 *        Verify each returns status = 0.
 *     3. OP_NEW_WINDOW(WIN_CONSOLE).  Verify status = 0, geometry
 *        is non-zero, handle ref is non-null.  Park handle in O8.
 *     4. OP_BIND_SURFACE(WSURF_CONSOLE) on the handle.  Verify
 *        status = 0 and the returned ref is non-null.
 *     5. OP_BIND_SURFACE(WSURF_KEYBOARD) on the handle.  Verify
 *        status = 0 and the returned ref is non-null.
 *     6. OP_BIND_SURFACE(WSURF_GRID) on the handle (a CONSOLE
 *        window — GRID is graphical-only).  Verify status = E_INVAL.
 *     7. OP_NEW_WINDOW(WIN_CONSOLE) a SECOND time.  Verify status =
 *        E_NOSPC — the milestone-1 daemon hardcodes N=1.
 *     8. OP_NEW_WINDOW(WIN_GRAPHICAL).  Verify status = E_NOTIMPL.
 *     9. OP_DESTROY_WINDOW on the handle (in O8).  Verify status = 0.
 *    10. OP_NEW_WINDOW(WIN_CONSOLE) once more.  Verify status = 0
 *        — slot reuse works.
 *
 * On any failure print "FAIL: <stage>" and exit non-zero.  On full
 * success print "wm_smoke: PASS" and exit 0.
 *
 * Boot environment (set up by test_wm_smoke.sh's --service args):
 *
 *     O3 = our data segment (preserved as O15 below)
 *     O4 = our self-service (full caps; we ReceiveQueueAttach below)
 *     O5 = some surface cap to register as "console"  — typically
 *          the terminal's idx-1 cap (16=1@9).  Identity is opaque to
 *          the WM; we just need a non-null ref to feed into
 *          OP_REGISTER_SURFACE.
 *     O6 = some surface cap to register as "keyboard" (16=2@9).
 *     O7 = WM main service ref (idx WM_SERVICE_INDEX = 1) — the
 *          recipient of OP_REGISTER_SURFACE / OP_NEW_WINDOW.
 *
 * OR hygiene
 * ----------
 * Same as kbd_echo / mouse_paint:
 *   - SENDs and queue-polls both overlay O1..O4, so we save the boot
 *     O2/O3/O4 once into O13/O15/O14 at startup and restore after
 *     every wire op.
 *   - The reply_cap (a R+S sub-cap of our self-service) lives in O11
 *     across the test — it's loaded into O3 just before each SEND
 *     and the poll's overlay clobbers O3, but restore_or_state pulls
 *     it back from O15 right after.  O11 is otherwise unused: this
 *     test doesn't touch the task stack and doesn't run task_init.
 *   - The window handle, once we have it, lives in O8 across the
 *     test — only touched by wire ops on the handle (BIND / DESTROY).
 */

#include "liborisc.h"

/* ---- WM wire constants — must match tools/devices/oriscwm. -------------- */

#define WM_OP_REGISTER_SURFACE  1
#define WM_OP_NEW_WINDOW        2

#define WM_OP_BIND_SURFACE      1
#define WM_OP_DESTROY_WINDOW    2
/* OP_SUBSCRIBE_EVENTS = 3 — not exercised in this smoke test. */

#define WIN_CONSOLE             1
#define WIN_GRAPHICAL           2

#define WSURF_CONSOLE           1
#define WSURF_KEYBOARD          2
#define WSURF_GRID              3

#define E_INVAL                (-1)
#define E_NOSPC                (-7)
#define E_NOTIMPL              (-8)

/* ---- Restore boot OPR refs after a SEND/poll overlay. ---------------- */

static void
restore_or_state(void)
{
	asm volatile("omov o2, o13");   /* boot stack ref */
	asm volatile("omov o3, o15");   /* boot data ref */
	asm volatile("omov o4, o14");   /* self-service for next poll */
}

/* ---- ReceiveQueueAttach on our self-service. ------------------------- */

static int
attach_self_queue(void)
{
	int status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 16\n"            /* depth = 16 */
		"call  #0x203\n"                /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Derive an R+S sub-cap of our self-service into O11.  Used as the
 * reply_cap on every WM op. */
static int
derive_reply_cap_o11(void)
{
	int status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 9\n"             /* R | S */
		"call  #0x103\n"                /* ObjDerive → O1 */
		"nop\n"
		"omov  o11, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r4"
	);
	return status;
}

/* ---- Generic poll-and-extract.  Returns the queue-poll status; on
 *      success writes the reply's R3..R5 through the out-pointers and
 *      restores boot OPRs.  Does NOT inspect O2; callers that care
 *      about the returned ref need their own bespoke poll asm
 *      (wm_new_window / wm_bind_surface below). ----------------------- */

static int
poll_reply(int *out_status, int *out_r4, int *out_r5)
{
	int poll_status, r3, r4, r5;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"            /* infinite */
		"call  #0x204\n"                /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0"
		: "=r"(poll_status), "=r"(r3), "=r"(r4), "=r"(r5)
		:
		: "r1", "r2", "r3", "r4", "r5"
	);
	*out_status = r3;
	*out_r4     = r4;
	*out_r5     = r5;
	restore_or_state();
	return poll_status;
}

/* ---- High-level WM ops ----------------------------------------------- */

static int
wm_register_surface_o5(int kind)
{
	/* Copy `kind` into r5 BEFORE setting r4 to the op constant.
	 * pcc may pick r4 to hold the `kind` input itself, in which
	 * case `addiu r4, r0, %0` would clobber kind before we
	 * read it.  Doing the r5 copy first (which reads from
	 * whatever register pcc chose) is safe regardless of
	 * pcc's register-allocation decision. */
	asm volatile(
		"omov  o1, o7\n"                /* WM main service */
		"omov  o2, o5\n"                /* the surface cap */
		"omov  o3, o11\n"               /* reply_cap */
		"addu  r5, %1, r0\n"            /* kind FIRST */
		"addiu r4, r0, %0\n"            /* op = REGISTER_SURFACE */
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_REGISTER_SURFACE), "r"(kind)
		: "r1", "r4", "r5", "r6", "r7"
	);
	int status, r4, r5;
	(void)poll_reply(&status, &r4, &r5);
	return status;
}

static int
wm_register_surface_o6(int kind)
{
	asm volatile(
		"omov  o1, o7\n"
		"omov  o2, o6\n"
		"omov  o3, o11\n"
		"addu  r5, %1, r0\n"            /* kind FIRST (see above) */
		"addiu r4, r0, %0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_REGISTER_SURFACE), "r"(kind)
		: "r1", "r4", "r5", "r6", "r7"
	);
	int status, r4, r5;
	(void)poll_reply(&status, &r4, &r5);
	return status;
}

/* SEND OP_NEW_WINDOW(type).  On status==0 ONLY parks the returned
 * handle ref in O8 and writes geometry through out-pointers.  On
 * failure O8 is left untouched. */
static int
wm_new_window(int type, int *out_geom_a, int *out_geom_b)
{
	asm volatile(
		"omov  o1, o7\n"
		"onull o2\n"
		"omov  o3, o11\n"
		"addu  r5, %1, r0\n"            /* type FIRST (see register_surface comment) */
		"addiu r4, r0, %0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_NEW_WINDOW), "r"(type)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int status, geom_a, geom_b, poll_status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		/* Stash the reply's O2 (the handle, when present) into the
		 * scratch OPR O9 — this preserves it across restore_or_state's
		 * O2 overwrite below.  We promote O9 → O8 from C only on
		 * success so a previously parked handle survives failed
		 * allocations. */
		"omov  o9, o2\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r2, r0"
		: "=r"(status), "=r"(geom_a), "=r"(geom_b), "=r"(poll_status)
		:
		: "r1", "r2", "r3", "r4", "r5"
	);
	restore_or_state();
	if (poll_status == 0 && status == 0) {
		asm volatile("omov o8, o9");
	}
	*out_geom_a = geom_a;
	*out_geom_b = geom_b;
	if (poll_status != 0) return poll_status;
	return status;
}

/* SEND OP_BIND_SURFACE(kind) on the handle in O8.  Returns the
 * status; out_cap_was_null is set to 1 if the daemon's reply O2 (the
 * resolved surface cap) was null, 0 otherwise. */
static int
wm_bind_surface(int kind, int *out_cap_was_null)
{
	asm volatile(
		"omov  o1, o8\n"                /* window handle */
		"onull o2\n"
		"omov  o3, o11\n"
		"addu  r5, %1, r0\n"            /* kind FIRST (see register_surface comment) */
		"addiu r4, r0, %0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_BIND_SURFACE), "r"(kind)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int status, cap_isn, poll_status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"oisn  %1, o2\n"                /* check returned ref BEFORE restore */
		"addu  %0, r3, r0\n"
		"addu  %2, r2, r0"
		: "=r"(status), "=r"(cap_isn), "=r"(poll_status)
		:
		: "r1", "r2", "r3", "r4"
	);
	restore_or_state();
	*out_cap_was_null = cap_isn;
	if (poll_status != 0) return poll_status;
	return status;
}

static int
wm_destroy_window(void)
{
	asm volatile(
		"omov  o1, o8\n"
		"onull o2\n"
		"omov  o3, o11\n"
		"addiu r4, r0, %0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_DESTROY_WINDOW)
		: "r1", "r4", "r5", "r6", "r7"
	);
	int status, r4, r5;
	(void)poll_reply(&status, &r4, &r5);
	return status;
}

/* OISN test on O8.  1 if null. */
static int
handle_is_null(void)
{
	int isn;
	asm volatile("oisn %0, o8" : "=r"(isn));
	return isn;
}

/* ---- The driver ------------------------------------------------------ */

static void
fail(const char *stage, int got)
{
	print_str("FAIL: ");
	print_str(stage);
	print_str(" got=");
	print_int(got);
	print_str("\n");
}

int
main(void)
{
	register void *__or o2_stack __asm__("o2");
	register void *__or o3_data  __asm__("o3");
	register void *__or o4_self  __asm__("o4");
	register void *__or o13      __asm__("o13");
	register void *__or o14      __asm__("o14");
	register void *__or o15      __asm__("o15");

	/* Save boot O2/O3/O4 into O13/O15/O14 — same convention as
	 * kbd_echo's restore_or_state.  These slots are also touched by
	 * print_str (data) / print_int (stack) so we have to leave them
	 * intact across calls. */
	o13 = o2_stack;
	o14 = o4_self;
	o15 = o3_data;

	int status;

	print_str("wm_smoke: starting\n");

	/* Step 1: attach the self-mailbox + derive reply_cap into O11. */
	status = attach_self_queue();
	if (status != 0) { fail("attach_self_queue", status); return 1; }
	status = derive_reply_cap_o11();
	if (status != 0) { fail("derive_reply_cap", status); return 1; }

	/* Step 2: register the two surfaces. */
	status = wm_register_surface_o5(WSURF_CONSOLE);
	if (status != 0) { fail("register CONSOLE", status); return 2; }
	status = wm_register_surface_o6(WSURF_KEYBOARD);
	if (status != 0) { fail("register KEYBOARD", status); return 2; }

	/* Step 3: allocate a CONSOLE window. */
	int geom_a = 0, geom_b = 0;
	status = wm_new_window(WIN_CONSOLE, &geom_a, &geom_b);
	if (status != 0)        { fail("new_window CONSOLE #1", status); return 3; }
	if (handle_is_null())   { fail("new_window handle null", 0); return 3; }
	if (geom_a == 0)        { fail("new_window geom_a zero", geom_a); return 3; }
	if (geom_b == 0)        { fail("new_window geom_b zero", geom_b); return 3; }
	print_str("wm_smoke: new_window CONSOLE OK (geom_a=");
	print_int(geom_a);
	print_str(", geom_b=");
	print_int(geom_b);
	print_str(")\n");

	/* Step 4 & 5: bind console + keyboard. */
	int cap_was_null;
	status = wm_bind_surface(WSURF_CONSOLE, &cap_was_null);
	if (status != 0)  { fail("bind CONSOLE", status); return 4; }
	if (cap_was_null) { fail("bind CONSOLE cap null", 0); return 4; }
	status = wm_bind_surface(WSURF_KEYBOARD, &cap_was_null);
	if (status != 0)  { fail("bind KEYBOARD", status); return 5; }
	if (cap_was_null) { fail("bind KEYBOARD cap null", 0); return 5; }
	print_str("wm_smoke: bind CONSOLE + KEYBOARD OK\n");

	/* Step 6: bind GRID on a CONSOLE window — must fail E_INVAL. */
	status = wm_bind_surface(WSURF_GRID, &cap_was_null);
	if (status != E_INVAL) { fail("bind GRID expected E_INVAL", status); return 6; }
	print_str("wm_smoke: bind GRID rejected (expected)\n");

	/* Step 7: second OP_NEW_WINDOW(CONSOLE) — must fail E_NOSPC. */
	int dummy_a = 0, dummy_b = 0;
	status = wm_new_window(WIN_CONSOLE, &dummy_a, &dummy_b);
	if (status != E_NOSPC) {
		fail("new_window CONSOLE #2 expected E_NOSPC", status); return 7;
	}
	print_str("wm_smoke: second CONSOLE refused (expected)\n");

	/* Step 8: OP_NEW_WINDOW(GRAPHICAL) — must fail E_NOTIMPL. */
	status = wm_new_window(WIN_GRAPHICAL, &dummy_a, &dummy_b);
	if (status != E_NOTIMPL) {
		fail("new_window GRAPHICAL expected E_NOTIMPL", status); return 8;
	}
	print_str("wm_smoke: GRAPHICAL not-implemented (expected)\n");

	/* Step 9: destroy the window from step 3.  Steps 7 and 8 both
	 * failed inside the daemon; wm_new_window's tail-asm skips its
	 * O8 stash on non-zero status, so O8 still holds the step-3
	 * handle. */
	status = wm_destroy_window();
	if (status != 0) { fail("destroy_window", status); return 9; }
	print_str("wm_smoke: destroy OK\n");

	/* Step 10: a fresh CONSOLE allocation should succeed. */
	status = wm_new_window(WIN_CONSOLE, &dummy_a, &dummy_b);
	if (status != 0) { fail("new_window CONSOLE #3", status); return 10; }
	print_str("wm_smoke: slot-reuse OK\n");

	print_str("wm_smoke: PASS\n");
	return 0;
}

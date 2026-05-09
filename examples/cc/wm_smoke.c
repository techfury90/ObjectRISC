/*
 * wm_smoke.c — smoke test for the oriscwm window-manager wire
 * protocol (milestone 2: .orx WM, single-service payload-dispatch).
 *
 * Wire-shape changes from milestone 1 (see ouroboros/oriscwm.c
 * docstring for the full protocol):
 *
 *   - All ops SEND to the WM's main service in O7 (no per-window-
 *     handle services any more).
 *   - Window targeting is via R4 = window_id rather than via the
 *     SEND target's idx.
 *   - OP_REGISTER_SURFACE is gone — the .orx WM walks oriscdir
 *     itself for /sys/term/0/{console,keyboard}.
 *
 * Register convention (this is what trips you up writing inline asm
 * for SEND-and-poll round-trips, so worth pinning):
 *
 *     SEND emits int_payload from sender's R4..R7.
 *     Queue dispatch on the receiver overlays int_payload into
 *     receiver's R3..R6.  So:
 *
 *         sender's R4 → receiver's R3   (op)
 *         sender's R5 → receiver's R4   (window_id)
 *         sender's R6 → receiver's R5   (arg: type / kind / notify_op)
 *         sender's R7 → receiver's R6   (unused in milestone 2)
 *
 *     Replies follow the same shift: WM's wm_reply puts (status,
 *     r5, r6, r7) into its R4..R7, which we receive at our R3..R6.
 *     In wm_new_window's reply specifically:
 *
 *         our R3 = status
 *         our R4 = geom_a   (packed w_px<<16 | h_px)
 *         our R5 = geom_b   (packed w_cells<<16 | h_cells)
 *         our R6 = window_id
 *
 * Test sequence:
 *
 *     1. Allocate self-mailbox + derive reply_cap (in O11) and
 *        owner ref (in O10).
 *     2. OP_NEW_WINDOW(WIN_TYPE_CONSOLE).  Verify status, geometry,
 *        window_id non-zero.  Save wid.
 *     3. OP_BIND_SURFACE(wid, WSURF_CONSOLE).
 *     4. OP_BIND_SURFACE(wid, WSURF_KEYBOARD).
 *     5. OP_BIND_SURFACE(wid, WSURF_GRID) → E_INVAL.
 *     6. OP_NEW_WINDOW(WIN_TYPE_CONSOLE) again → E_NOSPC.
 *     7. OP_NEW_WINDOW(WIN_TYPE_GRAPHICAL) → E_NOTIMPL.
 *     8. OP_DESTROY_WINDOW(wid) → ok.
 *     9. OP_NEW_WINDOW(WIN_TYPE_CONSOLE) → ok (slot reuse).
 *
 * Boot environment (set up by test_wm_smoke.sh's --service args):
 *     O3 = our data segment (preserved as O15)
 *     O4 = our self-service
 *     O7 = WM main service ref
 *
 * OR hygiene
 * ----------
 * Save boot O2/O3/O4 into O13/O15/O14 at startup.  Restore after
 * every wire op.  reply_cap lives in O11; owner-ref lives in O10.
 */

#include "liborisc.h"

/* ---- WM wire constants — must match ouroboros/oriscwm.c ----------- */

#define WM_OP_NEW_WINDOW         1
#define WM_OP_BIND_SURFACE       2
#define WM_OP_DESTROY_WINDOW     3
/* OP_SUBSCRIBE_EVENTS = 4 — not exercised in this smoke test */

#define WIN_TYPE_CONSOLE   1
#define WIN_TYPE_GRAPHICAL 2

#define WSURF_CONSOLE   1
#define WSURF_KEYBOARD  2
#define WSURF_GRID      3

#define E_INVAL   (-1)
#define E_NOSPC   (-7)
#define E_NOTIMPL (-8)

#define CAP_R 0x01
#define CAP_S 0x08

/* ---- Restore boot OPRs after a SEND/poll overlay. ---------------- */

static void
restore_or_state(void)
{
	asm volatile("omov o2, o13");   /* boot stack ref */
	asm volatile("omov o3, o15");   /* boot data ref */
	asm volatile("omov o4, o14");   /* self-service for next poll */
}

/* ---- Setup helpers ----------------------------------------------- */

static int
attach_self_queue(void)
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

static int
derive_reply_cap_o11(void)
{
	int status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, %1\n"
		"call  #0x103\n"
		"nop\n"
		"omov  o11, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_S)
		: "r1", "r2", "r4"
	);
	return status;
}

static int
derive_owner_cap_o10(void)
{
	int status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, %1\n"
		"call  #0x103\n"
		"nop\n"
		"omov  o10, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_S)
		: "r1", "r2", "r4"
	);
	return status;
}

/* ---- SEND helpers -----------------------------------------------
 *
 * Each helper sets up the SEND with op in R4, wid in R5, arg in R6,
 * and the appropriate O2 / O3 (reply_cap from O11, owner ref or
 * notify_cap as needed).  The trick from milestone 1 still applies:
 * pcc may pick R4/R5/R6 to hold input operands, so we copy inputs
 * into R5/R6 BEFORE setting R4 from a literal.
 */

/* OP_NEW_WINDOW: op=1, wid=0, arg=type, O2=owner ref. */
static int
wm_new_window(int type, int *out_geom_a, int *out_geom_b, int *out_wid)
{
	asm volatile(
		"omov  o1, o7\n"
		"omov  o2, o10\n"              /* owner ref */
		"omov  o3, o11\n"              /* reply_cap */
		"addu  r6, %1, r0\n"           /* arg = type (FIRST — see hygiene note) */
		"addiu r4, r0, %0\n"           /* op */
		"addiu r5, r0, 0\n"            /* wid = 0 */
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_NEW_WINDOW), "r"(type)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int poll_status, r3, r4, r5, r6;
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
		: "=r"(poll_status), "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	restore_or_state();
	if (poll_status != 0) return poll_status;
	*out_geom_a = r4;
	*out_geom_b = r5;
	*out_wid    = r6;
	return r3;   /* status */
}

/* OP_BIND_SURFACE: op=2, wid, arg=kind. */
static int
wm_bind_surface(int wid, int kind, int *out_cap_was_null)
{
	asm volatile(
		"omov  o1, o7\n"
		"onull o2\n"
		"omov  o3, o11\n"
		"addu  r5, %1, r0\n"           /* wid (FIRST) */
		"addu  r6, %2, r0\n"           /* kind */
		"addiu r4, r0, %0\n"           /* op */
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_BIND_SURFACE), "r"(wid), "r"(kind)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int status, cap_isn, poll_status;
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"oisn  %1, o2\n"               /* check returned ref BEFORE restore */
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

/* OP_DESTROY_WINDOW: op=3, wid. */
static int
wm_destroy_window(int wid)
{
	asm volatile(
		"omov  o1, o7\n"
		"onull o2\n"
		"omov  o3, o11\n"
		"addu  r5, %1, r0\n"           /* wid (FIRST) */
		"addiu r4, r0, %0\n"           /* op */
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "i"(WM_OP_DESTROY_WINDOW), "r"(wid)
		: "r1", "r4", "r5", "r6", "r7"
	);
	int poll_status, r3, r4, r5, r6;
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
		: "=r"(poll_status), "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	restore_or_state();
	if (poll_status != 0) return poll_status;
	return r3;
}

/* ---- The driver ------------------------------------------------- */

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

	o13 = o2_stack;
	o14 = o4_self;
	o15 = o3_data;

	int status;

	print_str("wm_smoke: starting\n");

	status = attach_self_queue();
	if (status != 0) { fail("attach_self_queue", status); return 1; }
	status = derive_reply_cap_o11();
	if (status != 0) { fail("derive_reply_cap", status); return 1; }
	status = derive_owner_cap_o10();
	if (status != 0) { fail("derive_owner_cap", status); return 1; }

	/* Step 2: allocate a CONSOLE window. */
	int geom_a = 0, geom_b = 0, wid = 0;
	status = wm_new_window(WIN_TYPE_CONSOLE, &geom_a, &geom_b, &wid);
	if (status != 0)         { fail("new_window CONSOLE #1", status); return 2; }
	if (wid < 1)             { fail("new_window wid invalid", wid); return 2; }
	if (geom_a == 0)         { fail("new_window geom_a zero", geom_a); return 2; }
	if (geom_b == 0)         { fail("new_window geom_b zero", geom_b); return 2; }
	print_str("wm_smoke: new_window CONSOLE OK (wid=");
	print_int(wid);
	print_str(", geom_a=");
	print_int(geom_a);
	print_str(", geom_b=");
	print_int(geom_b);
	print_str(")\n");

	/* Step 3 & 4: bind console + keyboard. */
	int cap_was_null;
	status = wm_bind_surface(wid, WSURF_CONSOLE, &cap_was_null);
	if (status != 0)  { fail("bind CONSOLE", status); return 3; }
	if (cap_was_null) { fail("bind CONSOLE cap null", 0); return 3; }
	status = wm_bind_surface(wid, WSURF_KEYBOARD, &cap_was_null);
	if (status != 0)  { fail("bind KEYBOARD", status); return 4; }
	if (cap_was_null) { fail("bind KEYBOARD cap null", 0); return 4; }
	print_str("wm_smoke: bind CONSOLE + KEYBOARD OK\n");

	/* Step 5: bind GRID on a CONSOLE window — must fail E_INVAL. */
	status = wm_bind_surface(wid, WSURF_GRID, &cap_was_null);
	if (status != E_INVAL) { fail("bind GRID expected E_INVAL", status); return 5; }
	print_str("wm_smoke: bind GRID rejected (expected)\n");

	/* Step 6: second CONSOLE allocation — must fail E_NOSPC. */
	int dummy_a = 0, dummy_b = 0, dummy_wid = 0;
	status = wm_new_window(WIN_TYPE_CONSOLE, &dummy_a, &dummy_b, &dummy_wid);
	if (status != E_NOSPC) {
		fail("new_window CONSOLE #2 expected E_NOSPC", status); return 6;
	}
	print_str("wm_smoke: second CONSOLE refused (expected)\n");

	/* Step 7: GRAPHICAL — must fail E_NOTIMPL. */
	status = wm_new_window(WIN_TYPE_GRAPHICAL, &dummy_a, &dummy_b, &dummy_wid);
	if (status != E_NOTIMPL) {
		fail("new_window GRAPHICAL expected E_NOTIMPL", status); return 7;
	}
	print_str("wm_smoke: GRAPHICAL not-implemented (expected)\n");

	/* Step 8: destroy. */
	status = wm_destroy_window(wid);
	if (status != 0) { fail("destroy_window", status); return 8; }
	print_str("wm_smoke: destroy OK\n");

	/* Step 9: re-allocate to confirm slot reuse. */
	int wid2 = 0;
	status = wm_new_window(WIN_TYPE_CONSOLE, &dummy_a, &dummy_b, &wid2);
	if (status != 0) { fail("new_window CONSOLE #3", status); return 9; }
	if (wid2 < 1)    { fail("new_window #3 wid invalid", wid2); return 9; }
	print_str("wm_smoke: slot-reuse OK (wid=");
	print_int(wid2);
	print_str(")\n");

	print_str("wm_smoke: PASS\n");
	return 0;
}

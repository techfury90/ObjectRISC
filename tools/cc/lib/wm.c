/*
 * wm.c — libc wrapper for the oriscwm window-manager wire protocol.
 *
 * Mirror of dir.c's shape: a lazy-init slot for the WM mailbox
 * sub-cap (WM_SLOT), populated on first call via dir_walk on
 * "/sys/wm/0", plus per-op helpers that SEND the wire op and poll
 * the per-program reply mailbox (REPLY_MB_SLOT, shared with sup.c
 * and dir.c — all three are synchronous SEND-and-poll, never
 * outstanding simultaneously).
 *
 * Public API:
 *
 *   int wm_init(void);
 *       Lazy: ensures WM_SLOT is populated.  Returns 0 OK,
 *       WM_NO_DIRECTORY (-6) if neither DIR_SLOT nor BOOT_PARENT_SLOT
 *       is set (so dir_walk can't bootstrap), WM_NO_WM (-2) if
 *       /sys/wm/0 doesn't resolve.
 *
 *   int wm_new_window(int type, int *out_wid,
 *                     int *out_w_cells, int *out_h_cells);
 *       SENDs OP_NEW_WINDOW.  Caller MUST place the owner-task
 *       ref in O1 before calling (the ref the WM will task_query
 *       to detect owner exit and auto-destroy).  Pass O1 = null
 *       to opt out of auto-destroy — the WM still allocates the
 *       window but won't watch it.
 *
 *   int wm_bind_surface(int wid, int kind);
 *       SENDs OP_BIND_SURFACE.  On success, the resolved surface
 *       cap is parked in WM_RESULT_SLOT (= DIR_RESULT_SLOT, same
 *       offset — the slot is generic "last-resolved-ref scratch").
 *       Caller follows up with `orefld oN, %0(o12)` :: "i"(...)
 *       to land it in the desired OPR slot.
 *
 *   int wm_destroy_window(int wid);
 *       SENDs OP_DESTROY_WINDOW.
 *
 *   int wm_subscribe_events(int wid, int notify_op);
 *       SENDs OP_SUBSCRIBE_EVENTS.  Caller MUST place the notify
 *       cap in O1 before calling (it gets passed in O4 of the
 *       wire SEND).  notify_op is 1..255.  Stub on the WM side
 *       for milestone 2 — accepted and stored, no events fire
 *       yet — but the wire shape is committed.
 *
 * Wire constants — must match ouroboros/oriscwm.c.  The user-
 * visible WIN_TYPE_* / WSURF_* / WIN_E_* live in liborisc.h so
 * client programs can include just that header.
 *
 * No-WM fallback
 * --------------
 * When /sys/wm/0 doesn't resolve (no oriscwm running), wm_init
 * returns WM_NO_WM and the slot stays null.  Subsequent calls to
 * wm_* short-circuit with the same error.  Programs that want the
 * "use the WM if present, fall back otherwise" pattern check
 * wm_init's status and use direct boot-OPR surfaces on miss —
 * exactly the same shape as the supervisor's directory-walk-OR-
 * keep-boot-wired pattern.
 */

#include "liborisc.h"

#define TAG_DATA           0x4102
#define TAG_SERVICE        0x4103

#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* Wire ops on the WM main service (must match oriscwm.c). */
#define WM_OP_NEW_WINDOW         1
#define WM_OP_BIND_SURFACE       2
#define WM_OP_DESTROY_WINDOW     3
#define WM_OP_SUBSCRIBE_EVENTS   4

/* WM error codes — also surface in liborisc.h.  Mirrored locally for
 * the no-WM fallback short-circuit. */
#define WM_NO_WM         (-2)     /* /sys/wm/0 didn't resolve */
#define WM_NO_DIRECTORY  (-6)     /* no oriscdir to walk through */

/* Slot offsets in O12.  Mirrored from task.c's central slot map.
 *
 *   544 BOOT_PARENT_SLOT     (boot O8 — stays put)
 *   552 REPLY_MB_SLOT        (per-program mailbox, shared with
 *                             sup.c / dir.c — synchronous SEND-and-
 *                             poll, never outstanding simultaneously)
 *   584 DIR_SLOT             (oriscdir mailbox sub-cap)
 *   608 DIR_REPLY_SCRATCH    (derived reply sub-cap, shared)
 *   616 DIR_RESULT_SLOT      (last-resolved-ref scratch — also
 *                             where wm_bind_surface parks the
 *                             returned surface cap)
 *   680 WM_SLOT              (WM mailbox sub-cap, this file)
 *   688 WM_INPUT_REF_SLOT    (caller's owner-task ref stashed at
 *                             wm_new_window entry; loaded into O2
 *                             of the wire SEND)
 */

#define BOOT_PARENT_SLOT_OFFSET   544
#define REPLY_MB_SLOT_OFFSET      552
#define DIR_SLOT_OFFSET           584
#define DIR_REPLY_SCRATCH_OFFSET  608
#define DIR_RESULT_SLOT_OFFSET    616
#define WM_SLOT_OFFSET            680
#define WM_INPUT_REF_SLOT_OFFSET  688

/* OISN-style probe of WM_SLOT.  Returns 1 if null. */
static int
wm_slot_isn(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		: "i"(WM_SLOT_OFFSET)
		: "r1"
	);
	return isn;
}

/* Allocate REPLY_MB_SLOT if it's null.  Mirror of dir.c's
 * dir_reply_mailbox_init.  Idempotent — first non-null check
 * fast-returns. */
static int
wm_reply_mailbox_init(void)
{
	int isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		: "i"(REPLY_MB_SLOT_OFFSET)
		: "r1"
	);
	if (!isn) return 0;

	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"            /* ObjAlloc → O1 */
		"nop\n"
		"orefst o1, %3(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C),
		  "i"(REPLY_MB_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	asm volatile(
		"orefld o1, %1(o12)\n"
		"addiu r4, r0, 4\n"
		"call  #0x203\n"            /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(REPLY_MB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Compose /sys/wm/<my_term>/0 into buf.  Multi-WM (one instance per
 * terminal, Phase 59 / WM γ.15) means each caller walks the WM that
 * serves its own terminal.  task_my_terminal_idx() returns -1 when
 * the caller has no terminal info (top-level oriscrun boots that
 * skip Phase-51 R4 wiring); fall back to terminal 0 for back-compat
 * with single-WM configurations and pre-multi-WM tests. */
static void
wm_render_path(char *buf)
{
	int idx = task_my_terminal_idx();
	if (idx < 0) idx = 0;
	const char prefix[] = "/sys/wm/";
	const char suffix[] = "/0";
	int p = 0, i;
	for (i = 0; prefix[i]; i++) buf[p++] = prefix[i];
	if (idx >= 100) {
		buf[p++] = '0' + (idx / 100);
		buf[p++] = '0' + ((idx / 10) % 10);
		buf[p++] = '0' + (idx % 10);
	} else if (idx >= 10) {
		buf[p++] = '0' + (idx / 10);
		buf[p++] = '0' + (idx % 10);
	} else {
		buf[p++] = '0' + idx;
	}
	for (i = 0; suffix[i]; i++) buf[p++] = suffix[i];
	buf[p] = '\0';
}

/* Lazy: ensure WM_SLOT is populated.  Strategy:
 *   1. If WM_SLOT is non-null, fast-return.
 *   2. Otherwise dir_walk("/sys/wm/<my_term>/0").  On LEAF success,
 *      the resolved ref is in DIR_RESULT_SLOT; OREFLD it into O1 and
 *      OREFST into WM_SLOT.
 *   3. On dir_walk failure: -6 (no directory) or -2 (path not
 *      found) propagate as WM_NO_DIRECTORY / WM_NO_WM. */
int
wm_init(void)
{
	if (!wm_slot_isn())
		return 0;     /* already populated */

	int kind;
	char rem[16];
	char path[24];
	wm_render_path(path);
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc == -6) return WM_NO_DIRECTORY;
	if (rc < 0)   return WM_NO_WM;
	if (kind != DIR_KIND_LEAF) return WM_NO_WM;

	/* Copy DIR_RESULT_SLOT → WM_SLOT. */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"orefst o1, %1(o12)"
		:
		: "i"(DIR_RESULT_SLOT_OFFSET),
		  "i"(WM_SLOT_OFFSET)
		: "r1"
	);
	return 0;
}

/* Send-and-poll skeleton.  Each helper sets up the outgoing SEND
 * (with R4 = op, R5/R6 = window-id / arg, O2/O3/O4 as appropriate)
 * then blocks on REPLY_MB_SLOT for the WM's reply.  Returns the
 * status from the reply (R3 of the dispatched message). */

/* Derive the reply sub-cap of REPLY_MB into DIR_REPLY_SCRATCH.
 * Used by every wm_* SEND. */
static int
wm_derive_reply_subcap(void)
{
	int status;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"addiu r4, r0, 9\n"            /* R | S */
		"call  #0x103\n"               /* ObjDerive → O1 */
		"nop\n"
		"orefst o1, %2(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(REPLY_MB_SLOT_OFFSET),
		  "i"(DIR_REPLY_SCRATCH_OFFSET)
		: "r1", "r2", "r4"
	);
	return status;
}

/* OP_NEW_WINDOW: SEND with R4=op, R5=0, R6=type, O2=owner ref
 * (loaded from WM_INPUT_REF_SLOT — caller put it there at entry),
 * O3=reply_cap.  Caller MUST place owner ref in O1 before calling.
 * Reply: R3=status, R4=geom_a, R5=geom_b, R6=window_id. */
int
wm_new_window(int type, int *out_wid,
              int *out_w_cells, int *out_h_cells)
{
	/* Stash caller's O1 (owner ref) into WM_INPUT_REF_SLOT
	 * IMMEDIATELY — same reason as dir_register's stash: wm_init
	 * and wm_reply_mailbox_init both clobber O1 internally. */
	asm volatile(
		"orefst o1, %0(o12)"
		:
		: "i"(WM_INPUT_REF_SLOT_OFFSET)
	);

	int rc = wm_init();
	if (rc != 0) return rc;
	rc = wm_reply_mailbox_init();
	if (rc != 0) return rc;
	rc = wm_derive_reply_subcap();
	if (rc != 0) return rc;

	/* SEND.
	 *   O1 = WM main service     (WM_SLOT)
	 *   O2 = owner ref           (WM_INPUT_REF_SLOT)
	 *   O3 = reply_cap           (DIR_REPLY_SCRATCH)
	 *   R4 = op                  (NEW_WINDOW = 1)
	 *   R5 = 0                   (wid unused for NEW_WINDOW)
	 *   R6 = type
	 */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"orefld o2, %1(o12)\n"
		"orefld o3, %2(o12)\n"
		"addu   r6, %4, r0\n"       /* type FIRST (pcc R-allocator hygiene) */
		"addiu  r4, r0, %3\n"       /* op */
		"addiu  r5, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SLOT_OFFSET),
		  "i"(WM_INPUT_REF_SLOT_OFFSET),
		  "i"(DIR_REPLY_SCRATCH_OFFSET),
		  "i"(WM_OP_NEW_WINDOW),
		  "r"(type)
		: "r1", "r4", "r5", "r6", "r7"
	);

	/* Block on reply.  R3=status, R4=geom_a, R5=geom_b, R6=wid. */
	int status, geom_a, geom_b, wid, poll_status;
	asm volatile(
		"orefld o1, %5(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0\n"
		"addu  %4, r6, r0"
		: "=r"(poll_status), "=r"(status),
		  "=r"(geom_a), "=r"(geom_b), "=r"(wid)
		: "i"(REPLY_MB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	if (poll_status != 0) return poll_status;
	if (status != 0)      return status;
	if (out_wid)     *out_wid     = wid;
	if (out_w_cells) *out_w_cells = (geom_b >> 16) & 0xFFFF;
	if (out_h_cells) *out_h_cells = geom_b & 0xFFFF;
	return 0;
}

/* OP_BIND_SURFACE: SEND with R4=op, R5=wid, R6=kind, O3=reply_cap.
 * Reply: R3=status, O2=resolved surface cap.  We park O2 into
 * DIR_RESULT_SLOT before restoring boot ORs so the caller can
 * OREFLD from there. */
int
wm_bind_surface(int wid, int kind)
{
	int rc = wm_init();
	if (rc != 0) return rc;
	rc = wm_reply_mailbox_init();
	if (rc != 0) return rc;
	rc = wm_derive_reply_subcap();
	if (rc != 0) return rc;

	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"orefld o3, %1(o12)\n"
		"addu   r5, %3, r0\n"           /* wid FIRST */
		"addu   r6, %4, r0\n"           /* kind */
		"addiu  r4, r0, %2\n"           /* op */
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SLOT_OFFSET),
		  "i"(DIR_REPLY_SCRATCH_OFFSET),
		  "i"(WM_OP_BIND_SURFACE),
		  "r"(wid), "r"(kind)
		: "r1", "r4", "r5", "r6", "r7"
	);

	/* Block on reply, parking returned O2 into DIR_RESULT_SLOT
	 * before restoring O2. */
	int status, poll_status;
	asm volatile(
		"orefld o1, %2(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"orefst o2, %3(o12)\n"          /* park resolved cap */
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(poll_status), "=r"(status)
		: "i"(REPLY_MB_SLOT_OFFSET),
		  "i"(DIR_RESULT_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4"
	);
	if (poll_status != 0) return poll_status;
	return status;
}

/* OP_DESTROY_WINDOW: SEND with R4=op, R5=wid. */
int
wm_destroy_window(int wid)
{
	int rc = wm_init();
	if (rc != 0) return rc;
	rc = wm_reply_mailbox_init();
	if (rc != 0) return rc;
	rc = wm_derive_reply_subcap();
	if (rc != 0) return rc;

	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"orefld o3, %1(o12)\n"
		"addu   r5, %3, r0\n"
		"addiu  r4, r0, %2\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SLOT_OFFSET),
		  "i"(DIR_REPLY_SCRATCH_OFFSET),
		  "i"(WM_OP_DESTROY_WINDOW),
		  "r"(wid)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int status, poll_status;
	asm volatile(
		"orefld o1, %2(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(poll_status), "=r"(status)
		: "i"(REPLY_MB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4"
	);
	if (poll_status != 0) return poll_status;
	return status;
}

/* OP_SUBSCRIBE_EVENTS: SEND with R4=op, R5=wid, R6=notify_op,
 * O4=notify_cap (caller MUST place in O1 at function entry —
 * we stash via WM_INPUT_REF_SLOT just like wm_new_window does
 * for the owner ref). */
int
wm_subscribe_events(int wid, int notify_op)
{
	asm volatile(
		"orefst o1, %0(o12)"
		:
		: "i"(WM_INPUT_REF_SLOT_OFFSET)
	);

	int rc = wm_init();
	if (rc != 0) return rc;
	rc = wm_reply_mailbox_init();
	if (rc != 0) return rc;
	rc = wm_derive_reply_subcap();
	if (rc != 0) return rc;

	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"orefld o3, %1(o12)\n"
		"orefld o4, %2(o12)\n"
		"addu   r5, %4, r0\n"
		"addu   r6, %5, r0\n"
		"addiu  r4, r0, %3\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SLOT_OFFSET),
		  "i"(DIR_REPLY_SCRATCH_OFFSET),
		  "i"(WM_INPUT_REF_SLOT_OFFSET),
		  "i"(WM_OP_SUBSCRIBE_EVENTS),
		  "r"(wid), "r"(notify_op)
		: "r1", "r4", "r5", "r6", "r7"
	);

	int status, poll_status;
	asm volatile(
		"orefld o1, %2(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(poll_status), "=r"(status)
		: "i"(REPLY_MB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4"
	);
	if (poll_status != 0) return poll_status;
	return status;
}

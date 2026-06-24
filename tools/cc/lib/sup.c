/*
 * sup.c — client side of the Ouroboros supervisor RPC.
 *
 * Phase 45a: the supervisor (a separate program — see
 * `ouroboros/supervisor.c`) owns the .orx loader + spawn machinery.
 * Programs that need to spawn children (the shell, primarily) SEND
 * a request to the supervisor's spawn service and wait for the
 * resulting task ref to come back. This file is the client API.
 *
 * Phase 4 (obj.h migration): the raw capability asm is gone — the
 * three wire ops now go through the obj.h handle API (obj_make_bytes
 * for the request object, obj_send_3or for the multi-OR send,
 * obj_recv_cap / obj_recv_cap_full for the cap-bearing replies,
 * obj_fetch_to_stack for the listing). Capabilities live in obj.h
 * handles, never in C `__or` values.
 *
 * Boot ABI:
 *
 *     O12 + SUP_SLOT_OFFSET = supervisor R+S sub-cap
 *                             (parked by task_init from boot O8;
 *                             null on programs that weren't launched
 *                             by a supervisor)
 *     O12 + REPLY_MB_SLOT_OFFSET = our reply mailbox (lazy-alloc'd
 *                             on first sup call; reused for every
 *                             subsequent call; shared with dir.c)
 *
 * Wire protocol (shell → supervisor), op=1 spawn:
 *     recipient = supervisor sub-cap
 *     O2 = TAG_DATA bytes object: path\0args\0cwd\0
 *     O3 = R+S sub-cap of our reply mailbox (send-only — the
 *          supervisor is the most privileged peer, so its reply path
 *          stays attenuated)
 *     R4 = op (1 = spawn, 2 = shutdown, 5 = list_tasks)
 *     R5 = byte length of the spawn request payload
 *     R6 = target_pid, R7 = terminal_idx + 1
 *
 * Reply (supervisor → us, SEND on the reply_cap):
 *     R3 = status (0 OK, negative for orx_spawn-style failures)
 *     O2 = the new task ref (or null on failure)
 *
 * The new task ref is enrolled into the libc task table via
 * obj_register_task → task_register_o1 so callers get back a normal
 * task_t handle and can use task_wait / task_kill / orx_unload on it
 * as before.
 */

#include "liborisc.h"
#include "obj.h"

#define SPAWN_REQ_BUF_SIZE 256        /* == OBJ_BYTES_MAX, fits obj_make_bytes */

/* CAP bits (mirrored from the rest of libc — there's no central
 * header that exports these). */
#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* Byte offsets within O12 for the slots sup.c reaches for. task.c
 * keeps the canonical definitions; mirrored here as private
 * constants. SUP_SLOT is parked by task_init from boot O8;
 * REPLY_MB sits 8 bytes past it (shared with dir.c). DIR_INPUT_REF
 * is the "caller-supplied ref" scratch slot (obj_adopt_slot knows
 * it) — sup_list_tasks stashes its O1 recipient there at entry, the
 * same way dir_register stashes its ref-to-publish. */
#define SUP_SLOT_OFFSET            544
#define REPLY_MB_SLOT_OFFSET       552
#define DIR_INPUT_REF_SLOT_OFFSET  624

/* Stack VA base (CONTRACT.md §2): obj_fetch_to_stack lands bytes into
 * the boot stack object (O11) at `dst - STACK_BOTTOM`, so the caller's
 * `dst` must be a stack pointer. cmd_ps passes a local array. */
#define STACK_BOTTOM 0x001f0000

/* Test whether the supervisor sub-cap was provided at boot.
 * The "0=0@0" pad service-spec produces a literal-zero ref
 * (Phase 45a tweak in simorisc), so OISN works directly. */
int
sup_have_supervisor(void)
{
	int isn;
	asm volatile(
		"orefld o1, 544(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		:
		: "r1"
	);
	return !isn;
}

/* Ensure the per-program reply mailbox exists in REPLY_MB_SLOT. Lazy-
 * alloc on first call; subsequent calls are a single OREFLD + OISN.
 * Kept as asm (a raw-slot bootstrap, like dir.c's
 * dir_reply_mailbox_init) — the mailbox lives in slot 552 so dir.c
 * and sup.c can share it; each op adopts it into a handle per-call. */
static int
sup_reply_mailbox_init(void)
{
	int isn;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn) : : "r1"
	);
	if (!isn) return 0;          /* already alloc'd */

	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"          /* TAG_SERVICE */
		"addiu r6, r0, %2\n"          /* R|W|S|V|C */
		"call  #0x100\n"              /* ObjAlloc → O1 */
		"nop\n"
		"orefst o1, 552(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(0x4103),                /* TAG_SERVICE */
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* ReceiveQueueAttach(O1=mailbox, R4=depth=4). 4 is plenty —
	 * sup ops are strictly request/response, never queue. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 4\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Restore the boot stack / data refs (O11/O15) into O2/O3 after an
 * obj.h send/recv (which use O2/O3 as scratch) so a following
 * term_print of a stack/data-segment string reads through the right
 * refs. Mirrors dir.c's _dir_restore_or. */
static void
_sup_restore_or(void)
{
	asm volatile("omov o2, o11\n omov o3, o15");
}

/* Pack `path\0args\0cwd\0` into `dst`. Returns the total byte count
 * written (including the three NULs). */
static int
sup_pack_request(char *dst, const char *path,
                 const char *args, const char *cwd)
{
	int n = 0;
	if (path)
		while (n + 1 < SPAWN_REQ_BUF_SIZE && path[n]) {
			dst[n] = path[n]; n++;
		}
	dst[n++] = '\0';
	int i;
	if (args) {
		i = 0;
		while (n + 1 < SPAWN_REQ_BUF_SIZE && args[i]) {
			dst[n++] = args[i++];
		}
	}
	dst[n++] = '\0';
	if (cwd) {
		i = 0;
		while (n + 1 < SPAWN_REQ_BUF_SIZE && cwd[i]) {
			dst[n++] = cwd[i++];
		}
	}
	dst[n++] = '\0';
	return n;
}

/* sup_spawn — RPC the supervisor to load + TaskCreate a `.orx` and
 * return a libc task_t referring to the resulting task. Returns a
 * negative status code from the supervisor (mirrors orx_spawn's
 * error codes), or -1 if our local task table is full. */
task_t
sup_spawn(const char *path, const char *args, const char *cwd)
{
	/* Phase 51: SUP_TARGET_ANY (= round-robin OK) by default. The
	 * supervisor spreads load across live CPUs and the receiver's
	 * pass-through (Phase 49) routes the child's terminal output
	 * back to the requester's terminal regardless of host CPU.
	 * Callers wanting strict local placement use sup_spawn_at(
	 * SUP_TARGET_LOCAL, ...) explicitly; sup_spawn_at(N, ...) with
	 * a literal procid still pins to that CPU. */
	return sup_spawn_at(SUP_TARGET_ANY, path, args, cwd);
}

task_t
sup_spawn_at(int target_pid, const char *path,
             const char *args, const char *cwd)
{
	/* Fallback path: caller wasn't launched by a supervisor (boot
	 * O8 was null → SUP_SLOT is null). Hand off to orx_spawn
	 * directly — same code that orx_run uses. target_pid is
	 * irrelevant in this mode (no supervisor to relay to). */
	if (!sup_have_supervisor())
		return orx_spawn(path, args, cwd);

	if (obj_init() != 0)
		return -1;

	int status = sup_reply_mailbox_init();
	if (status != 0)
		return status;

	/* Pack path\0args\0cwd\0 on the stack, then stage it into a fresh
	 * TAG_DATA request object (single-shot per spawn so concurrent
	 * shells can't collide). SPAWN_REQ_BUF_SIZE == OBJ_BYTES_MAX, so
	 * it fits obj_make_bytes without a cap bump. */
	char reqbuf[SPAWN_REQ_BUF_SIZE];
	int payload_len = sup_pack_request(reqbuf, path, args, cwd);

	/* Build the request + caps with a SEND-time peak of 3 handles. The
	 * handle table (OBJ_NHANDLE=16) also holds the CALLER's persistent
	 * handles, and the callers here are fat — login ~5 (term + host_io +
	 * grid), the WM-session shell ~6 — so even at peak 3 we'd hit 9 > 8,
	 * which is exactly why the table grew 8->16 (see obj.h). Keep the peak
	 * minimal anyway, as headroom. Order to stay at 3: derive the
	 * attenuated reply sub-cap, DROP the mailbox handle, then alloc the
	 * request + recipient; re-adopt the mailbox for the poll. */
	obj_t reply_h      = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	/* Derive a SEND-ONLY (R+S) sub-cap of our reply mailbox — the
	 * supervisor only needs to SEND its reply, never to read/own our
	 * mailbox. Keeps the most-privileged peer on an attenuated reply
	 * path (capability hygiene). */
	obj_t reply_send_h = obj_derive(reply_h, CAP_R | CAP_S);
	obj_drop(reply_h);                 /* re-adopted after the SEND */

	obj_t req_h = obj_make_bytes(reqbuf, payload_len,
	                             CAP_R | CAP_W | CAP_V | CAP_C);
	obj_t sup_h = obj_adopt_slot(SUP_SLOT_OFFSET);
	if (reply_send_h < 0 || req_h < 0 || sup_h < 0) {
		if (reply_send_h >= 0) obj_drop(reply_send_h);
		if (req_h >= 0)        obj_free(req_h);
		obj_drop(sup_h);
		return -1;
	}

	/* Phase 51: pack our terminal_idx into R7 so the supervisor can
	 * route a relayed spawn back to the right oriscterm. Encoding:
	 *   0   = "no terminal info" (top-level boot, legacy callers)
	 *   N+1 = "this requester is on terminal N" */
	int term_idx = task_my_terminal_idx();
	int term_hint_plus_one = (term_idx >= 0) ? (term_idx + 1) : 0;

	/* SEND op=1 to the supervisor.
	 *   O1 = supervisor sub-cap (the LOCAL supervisor relays to a peer
	 *        when target_pid names a different CPU)
	 *   O2 = request bytes, O3 = attenuated reply sub-cap
	 *   R5 = payload length, R6 = target_pid, R7 = terminal hint */
	obj_send_3or(sup_h, req_h, reply_send_h, OBJ_NULL,
	             1, payload_len, target_pid, term_hint_plus_one);
	/* The service cap + the reply-send sub-cap are dead the instant the
	 * SEND returns; drop them now (the dir_walk fix). */
	obj_drop(sup_h);
	obj_drop(reply_send_h);

	/* Re-adopt the mailbox (the reply lands in its queue regardless of
	 * which handle names it) and block on the reply. R3 = spawn status;
	 * O2 = the new task ref (a cap, null on failure). */
	reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	if (reply_h < 0) { obj_free(req_h); return -1; }
	int spawn_status = 0;
	obj_t task_h = obj_recv_cap(reply_h, &spawn_status);
	_sup_restore_or();

	/* The reply is the barrier proving the supervisor fetched the
	 * request bytes, so the request object can be freed now — no
	 * ObjFreeDeferred drain-window hack. */
	obj_free(req_h);
	obj_drop(reply_h);

	if (task_h < 0)                /* poll failed, or spawn failed (null cap) */
		return (spawn_status != 0) ? spawn_status : -1;

	/* Enroll the new task's ref (in task_h) into the libc task table. */
	task_t result = obj_register_task(task_h);
	obj_drop(task_h);
	return result;
}

/* sup_shutdown — fire-and-forget op=2 SEND telling the supervisor
 * "I'm about to TaskExit, you can wind down too." No reply. The
 * shell calls this from its `exit`/`quit` path right before
 * returning out of main() (which crt0 lowers to TaskExit).
 *
 * Why we need it: simorisc's finite-timeout ReceiveQueuePoll only
 * decrements its tick counter when the polling task is the current
 * task on its CPU. A supervisor blocked on poll while the shell
 * runs (and then exits) would never wake — the timeout would never
 * count down. An explicit SEND delivers a real message into the
 * supervisor's queue, satisfying the wake condition deterministically.
 *
 * No-op when there's no supervisor (program launched directly by
 * oriscrun, validation tests, etc.) — same fallback gate as
 * sup_spawn. */
void
sup_shutdown(void)
{
	if (!sup_have_supervisor()) return;
	if (obj_init() != 0) return;

	obj_t sup_h = obj_adopt_slot(SUP_SLOT_OFFSET);
	if (sup_h < 0) return;

	obj_send_3or(sup_h, OBJ_NULL, OBJ_NULL, OBJ_NULL, 2, 0, 0, 0);
	obj_drop(sup_h);
	_sup_restore_or();
}

/* --- Phase 52: SUP_OP_LIST_TASKS client side ----------------------
 *
 * sup_list_tasks(dst, max) → byte length on success, negative on
 * failure. SENDs op=5 to the supervisor whose R+S sub-cap the caller
 * OREFLD'd into O1 immediately before calling (a sub-cap of the
 * desired supervisor's spawn mailbox — caller obtains it via
 * dir_walk("/sys/cpu/<N>/supervisor"); same O1-passing convention
 * dir_register uses). For `ps` the shell walks all peers itself and
 * SENDs directly, no relay — each peer's reply comes back to OUR
 * reply mailbox, ObjFetchBytes-tunneled if the peer is on another CPU.
 *
 * The reply carries a TAG_DATA listing ref in O2 and R4 = byte count.
 * obj_recv_cap_full lands the ref in a handle; obj_fetch_to_stack
 * copies it into `dst` (capped at `max`). ObjFetchBytes (inside
 * obj_fetch_to_stack) handles BOTH local and remote bytes refs —
 * MapObject would return ERR_EREMOTE for a peer's remote-home ref.
 *
 * Returns the byte length (0..max) on success, negative on failure
 * (-1 recipient null, -2 mailbox alloc, -3 handle alloc, -4 poll,
 * -5 fetch, -6 obj_init; other negatives = supervisor-reported). */
int
sup_list_tasks(char *dst, int max)
{
	/* Stash the caller-supplied recipient cap (in O1) to the input-ref
	 * slot before anything clobbers O1, and verify it's non-null. */
	int isn;
	asm volatile(
		"orefst o1, 624(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn) : : "memory"
	);
	if (isn) return -1;                 /* null recipient */

	if (obj_init() != 0) return -6;

	int status = sup_reply_mailbox_init();
	if (status != 0) return -2;

	/* Same low-peak ordering as sup_spawn: derive the attenuated reply
	 * sub-cap, drop the mailbox handle, adopt the recipient, SEND, then
	 * re-adopt the mailbox for the reply poll (SEND-time peak 2). */
	obj_t reply_h      = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t reply_send_h = obj_derive(reply_h, CAP_R | CAP_S);
	obj_drop(reply_h);
	obj_t sup_h        = obj_adopt_slot(DIR_INPUT_REF_SLOT_OFFSET);
	if (sup_h < 0 || reply_send_h < 0) {
		if (reply_send_h >= 0) obj_drop(reply_send_h);
		obj_drop(sup_h);
		return -3;
	}

	/* SEND op=5: O1 = recipient, O2 = null, O3 = attenuated reply sub-cap. */
	obj_send_3or(sup_h, OBJ_NULL, reply_send_h, OBJ_NULL, 5, 0, 0, 0);
	obj_drop(sup_h);
	obj_drop(reply_send_h);

	/* Reply: R3 = status, R4 = byte length, O2 = TAG_DATA listing ref. */
	reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	if (reply_h < 0) return -3;
	int rep[4];
	obj_t listing_h = obj_recv_cap_full(reply_h, rep);
	_sup_restore_or();
	obj_drop(reply_h);
	if (listing_h < 0) return -4;       /* poll itself failed */

	int reply_status = rep[0];
	int length       = rep[1];
	if (reply_status != 0) { obj_drop(listing_h); return reply_status; }
	if (length == 0)       { obj_drop(listing_h); return 0; }

	int copy_len = (length < max) ? length : max;
	int dst_off  = (int)((unsigned int)dst - STACK_BOTTOM);
	int fs = obj_fetch_to_stack(listing_h, dst_off, copy_len);
	_sup_restore_or();
	obj_drop(listing_h);
	if (fs != 0) return -5;
	return copy_len;
}

/*
 * sup.c — client side of the Ouroboros supervisor RPC.
 *
 * Phase 45a: the supervisor (a separate program — see
 * `ouroboros/supervisor.c`) owns the .orx loader + spawn machinery.
 * Programs that need to spawn children (the shell, primarily) SEND
 * a request to the supervisor's spawn service and wait for the
 * resulting task ref to come back. This file is the client API.
 *
 * Boot ABI used by sup_spawn:
 *
 *     O12 + SUP_SLOT_OFFSET = supervisor R+S sub-cap
 *                             (parked by task_init from boot O8;
 *                             null on programs that weren't launched
 *                             by a supervisor)
 *     O12 + REPLY_MB_SLOT_OFFSET = our reply mailbox (lazy-alloc'd
 *                             on first sup_spawn call; reused for
 *                             every subsequent call)
 *
 * Wire protocol (shell → supervisor):
 *     recipient = supervisor sub-cap
 *     O2 = TAG_DATA bytes object: path\0args\0cwd\0
 *     O3 = R+S sub-cap of our reply mailbox
 *     R4 = op (1 = spawn; reserved 0/2/3 for kill/wait/etc later)
 *     R5 = byte length of the spawn request payload
 *
 * Reply (supervisor → us, SEND on the reply_cap):
 *     R4 = status (0 OK, negative for orx_spawn-style failures)
 *     O2 = the new task ref (or null on failure)
 *
 * The new task ref is dropped into the libc task table via
 * task_register_o1 so callers get back a normal task_t handle and
 * can use task_wait / task_kill / orx_unload on it as before. The
 * task is local (it lives on the same CPU as the supervisor, which
 * for 45a is the same CPU as the caller); for 45b's multi-CPU
 * future the ref will encode a remote home and the firmware
 * primitives will forward over the wire transparently.
 */

#include "liborisc.h"

#define SPAWN_REQ_BUF_SIZE 256
#define TAG_DATA           0x4102
#define TAG_SERVICE        0x4103

/* CAP bits (mirrored from the rest of libc — there's no central
 * header that exports these). */
#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* Byte offsets within O12 for the slots sup.c uses.
 * SUP_SLOT_OFFSET is set in task.c (where task_init parks O8 at
 * boot). REPLY_MB_SLOT_OFFSET sits 8 bytes past it. Mirror them
 * here as private constants — task.c keeps the canonical
 * definitions. */
#define SUP_SLOT_OFFSET       544
#define REPLY_MB_SLOT_OFFSET  552

/* Stash for derived reply sub-caps. Mirrors dir.c's
 * DIR_REPLY_SCRATCH_OFFSET (and shares the same physical slot —
 * sup.c and dir.c are both synchronous SEND-and-poll clients that
 * never have a reply outstanding simultaneously, so they're safe
 * to share). Replaces the 45a-era pattern of parking the derived
 * reply sub-cap in O15: that slot is task.c's boot data ref save,
 * and term.c's _term_restore_or relies on it being intact for
 * any subsequent term_print of a data-segment string. Phase 45g
 * bugfix. */
#define SUP_REPLY_SCRATCH_OFFSET  608

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

/* Ensure the per-program reply mailbox exists. Lazy-alloc on first
 * call; subsequent calls are a single OREFLD + OISN. */
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
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* ReceiveQueueAttach(O1=mailbox, R4=depth=4). 4 is plenty —
	 * sup_spawn is strictly request/response, never queues. */
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

/* Pack `path\0args\0cwd\0` into the buffer at `va`. Returns the
 * total byte count written (including the three NULs). */
static int
sup_pack_request(unsigned int va, const char *path,
                 const char *args, const char *cwd)
{
	char *dst = (char *)va;
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
 * return a libc task_t referring to the resulting task. Returns
 * negative status code from the supervisor (mirrors orx_spawn's
 * error codes), or `task_register_o1`'s -1 if our local task
 * table is full.
 *
 * Single-shot: allocates a fresh TAG_DATA bytes object per call to
 * carry path/args/cwd, and a fresh R+S sub-cap from the reply
 * mailbox. The bytes object is freed via ObjFreeDeferred (~ a few
 * spawns will leak it for a brief drain window before reclamation).
 */
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
	int status;

	/* Fallback path: caller wasn't launched by a supervisor (boot
	 * O8 was null → SUP_SLOT is null). Hand off to orx_spawn
	 * directly — same code that orx_run uses. The target_pid
	 * argument is irrelevant in this mode (there's no supervisor
	 * to relay to); the spawn happens on the caller's CPU. */
	if (!sup_have_supervisor())
		return orx_spawn(path, args, cwd);

	status = sup_reply_mailbox_init();
	if (status != 0) return status;

	/* Allocate a fresh request bytes object — single-shot per
	 * spawn so concurrent shells can't collide. ObjAlloc, MapObject
	 * R+W into our own VA temporarily, write bytes, Unmap. */
	int payload_len;
	asm volatile(
		"addiu r4, r0, %1\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x100\n"               /* ObjAlloc → O1 */
		"nop\n"
		"omov  o14, o1\n"              /* O14 = bytes ref (long enough) */
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(SPAWN_REQ_BUF_SIZE), "i"(TAG_DATA),
		  "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* Map at 0x600000 (above ARGS_PARENT_VA at 0x500000 — distinct
	 * so we don't conflict with the long-lived argv mapping). */
	asm volatile(
		"omov  o1, o14\n"
		"lui   r4, 0x60\n"             /* va = 0x600000 */
		"addu  r5, r0, r0\n"
		"addiu r6, r0, %1\n"           /* R+W */
		"addiu r7, r0, %2\n"           /* length */
		"call  #0x110\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_W), "i"(SPAWN_REQ_BUF_SIZE)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) return status;

	payload_len = sup_pack_request((unsigned int)0x600000,
	                                path, args, cwd);

	/* Unmap. Bytes are now in the object's storage, fetched by
	 * the supervisor via OBJ_READ_REQ later. */
	asm volatile(
		"lui   r4, 0x60\n"
		"addiu r5, r0, %1\n"
		"call  #0x111\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(SPAWN_REQ_BUF_SIZE)
		: "r2", "r3", "r4", "r5"
	);
	if (status != 0) return status;

	/* Derive R+S sub-cap from our reply mailbox. Park into
	 * SUP_REPLY_SCRATCH (a slot in O12) rather than O15 — O15 is
	 * task.c's boot data ref save, which term.c restores from on
	 * every print. Stomping it here would corrupt every subsequent
	 * term_print of a data-segment string (manifests as
	 * "[read failed: flags=0x02]" — RESP_BOUNDS — at the terminal
	 * because oriscterm's OBJ_READ_REQ now reads from a tiny
	 * service mailbox instead of the real data segment). Phase 45g
	 * bugfix. */
	asm volatile(
		"orefld o1, 552(o12)\n"        /* full mailbox ref */
		"addiu r4, r0, 9\n"            /* R|S */
		"call  #0x103\n"               /* ObjDerive → O1 */
		"nop\n"
		"orefst o1, %1(o12)\n"         /* SUP_REPLY_SCRATCH */
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(SUP_REPLY_SCRATCH_OFFSET)
		: "r1", "r2", "r4"
	);
	if (status != 0) return status;

	/* SEND to the supervisor.
	 *   O1 = supervisor sub-cap (recipient — the LOCAL supervisor;
	 *        if target_pid names a different CPU, the supervisor
	 *        relays to its peer)
	 *   O2 = bytes ref (request payload, parked in O14)
	 *   O3 = reply sub-cap (loaded from SUP_REPLY_SCRATCH)
	 *   R4 = op = 1 (spawn)
	 *   R5 = payload length
	 *   R6 = target_pid (Phase 45e — SUP_TARGET_LOCAL or a literal
	 *        PROCID; the supervisor checks against its own PROCID
	 *        and relays via op=1 SEND to a peer when they differ) */
	/* Phase 51: pack our terminal_idx into R7 so the supervisor can
	 * route a relayed spawn back to the right oriscterm. Encoding:
	 *   0   = "no terminal info" (top-level boot, legacy callers)
	 *   N+1 = "this requester is on terminal N"
	 * The supervisor's handle_spawn_request decodes back to N, sets
	 * ORX_SLOT_CHILD_O5/O6/O7 from /sys/term/<N>/*, and threads N
	 * into the child's libc via the orx_task_create R5 → child.R4
	 * → _orisc_init_r4 chain. */
	int term_idx = task_my_terminal_idx();
	int term_hint_plus_one = (term_idx >= 0) ? (term_idx + 1) : 0;
	asm volatile(
		"orefld o1, 544(o12)\n"        /* supervisor sub-cap */
		"omov   o2, o14\n"
		"orefld o3, %3(o12)\n"         /* SUP_REPLY_SCRATCH */
		"addiu  r4, r0, 1\n"
		"addu   r5, %0, r0\n"
		"addu   r6, %1, r0\n"
		"addu   r7, %2, r0\n"          /* terminal_idx + 1 */
		"send   o1\n"
		: : "r"(payload_len), "r"(target_pid),
		    "r"(term_hint_plus_one),
		    "i"(SUP_REPLY_SCRATCH_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);

	/* Free the request bytes object — supervisor will OBJ_READ
	 * the bytes asynchronously, so use ObjFreeDeferred with a
	 * generous drain window. */
	asm volatile(
		"omov  o1, o14\n"
		"addiu r4, r0, 1500\n"         /* 1500ms drain */
		"call  #0x107\n"               /* ObjFreeDeferred */
		"nop"
		: : : "r2", "r3", "r4"
	);

	/* Block on our reply mailbox for the supervisor's response.
	 * R3 carries the spawn status (negative on failure). The new
	 * task ref arrives in O2. Move it into O1 so task_register_o1
	 * can pick it up. */
	int spawn_status;
	asm volatile(
		"orefld o1, 552(o12)\n"        /* mailbox */
		"addiu r4, r0, -1\n"           /* infinite timeout */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"omov  o1, o2\n"               /* the new task ref */
		"addu  %0, r3, r0\n"           /* R3 = caller's R4 */
		"addu  %1, r2, r0"             /* R2 = poll status */
		: "=r"(spawn_status), "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	if (status != 0) return -1;        /* poll itself failed */
	if (spawn_status != 0) return spawn_status;

	return task_register_o1();
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
 * count down. An explicit SEND from the shell delivers a real
 * message into the supervisor's queue, satisfying the wake
 * condition deterministically. (See supervisor.c's poll_one_request
 * comment for the full design note.)
 *
 * No-op when there's no supervisor (program launched directly by
 * oriscrun, validation tests, etc.) — same fallback gate as
 * sup_spawn. */
void
sup_shutdown(void)
{
	if (!sup_have_supervisor()) return;

	asm volatile(
		"orefld o1, 544(o12)\n"        /* supervisor sub-cap */
		"onull  o2\n"
		"onull  o3\n"
		"addiu  r4, r0, 2\n"           /* op = shutdown */
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : : "r1", "r4", "r5", "r6", "r7"
	);
}

/* --- Phase 52: SUP_OP_LIST_TASKS client side ---------------------- *
 *
 * sup_list_tasks(target_recipient, dst, max) → byte length on success,
 *                                              negative on failure.
 *
 * SENDs op=5 to the supervisor at `target_recipient` (a sub-cap of
 * the desired supervisor's spawn mailbox — caller obtains it via
 * dir_walk("/sys/cpu/<N>/supervisor")). Awaits a reply on our
 * lazily-alloc'd reply mailbox; on success the reply carries a
 * TAG_DATA bytes ref in O2 with a human-readable task listing and
 * R3 = byte count. We MapObject R-only at LIST_FETCH_VA, copy into
 * `dst` (capped at `max`), and Unmap.
 *
 * This is a free function rather than a wrapper around sup_spawn_at
 * because the caller wants to address a SPECIFIC peer, not the
 * "default" local supervisor with relay — for `ps` we walk all the
 * peers ourselves and SEND directly, no relay. Each peer's reply
 * comes back to OUR reply mailbox, OBJ_READ_REQ-tunneled if the
 * peer is on another CPU.
 *
 * Returns the actual byte length (0..max) on success, negative on
 * failure (-1 = recipient null, -2 = mailbox alloc failed, others
 * = supervisor-reported status).
 *
 * The recipient MUST be a non-null R+S sub-cap to a supervisor's
 * spawn mailbox. Pass it in O14 via the calling convention — see
 * the asm block below.
 */

/* Use ObjFetchBytes (#0x108) instead of MapObject for the reply
 * payload — it handles BOTH local and remote bytes refs through
 * the same call (firmware issues OBJ_READ_REQ on remote home),
 * whereas MapObject only works for local-home refs. The shell's
 * cross-CPU `ps` SENDs op=5 to peer supervisors, and their reply
 * bytes refs are remote-home → MapObject would return ERR_EREMOTE,
 * so we must use ObjFetchBytes here.
 *
 * Destination is the caller's stack — `dst_va - STACK_BOTTOM` gives
 * the byte offset into the boot stack object (parked in O11 by
 * task_init). Limitation: this only works when `dst` is a stack
 * pointer (caller's local array). Heap or data-segment buffers
 * would need a different ref. Shell's cmd_ps allocates a local
 * `char reply_buf[N]` on its stack, so this works. */
#define STACK_BOTTOM 0x001f0000

static int
sup_list_fetch_reply(char *dst, int length)
{
	int dst_offset = (int)((unsigned int)dst - STACK_BOTTOM);
	int status;
	asm volatile(
		"omov  o1, o14\n"           /* source = reply bytes ref */
		"omov  o2, o11\n"           /* destination = boot stack */
		"addiu r4, r0, 0\n"         /* src offset */
		"addu  r5, %1, r0\n"        /* dst offset */
		"addu  r6, %2, r0\n"        /* byte count */
		"call  #0x108\n"            /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(dst_offset), "r"(length)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* Block on our reply mailbox for the supervisor's response to op=5.
 * R3 carries the status, R4 the byte length, O2 the bytes ref. We
 * stash O2 into O14 for the caller to MapObject. Returns: status in
 * out_status, byte length in out_length. */
static int
sup_list_tasks_recv(int *out_status, int *out_length)
{
	int status, length, poll_status;
	asm volatile(
		"orefld o1, 552(o12)\n"        /* mailbox */
		"addiu r4, r0, -1\n"           /* infinite timeout */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"omov  o14, o2\n"              /* stash bytes ref */
		"addu  %0, r3, r0\n"           /* status */
		"addu  %1, r4, r0\n"           /* length */
		"addu  %2, r2, r0"             /* poll status */
		: "=r"(status), "=r"(length), "=r"(poll_status)
		:
		: "r1", "r2", "r3", "r4"
	);
	if (poll_status != 0) return poll_status;
	*out_status = status;
	*out_length = length;
	return 0;
}

/* Send op=5 to the supervisor sub-cap held in O14. Reply mailbox
 * sub-cap (in SUP_REPLY_SCRATCH from caller) goes in O3. */
static void
sup_list_tasks_send(void)
{
	asm volatile(
		"omov   o1, o14\n"             /* recipient */
		"onull  o2\n"
		"orefld o3, %0(o12)\n"         /* reply sub-cap */
		"addiu  r4, r0, 5\n"           /* op = LIST_TASKS */
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(SUP_REPLY_SCRATCH_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

/* Park the supervisor recipient ref into O14 and verify it's
 * non-null. Returns 0 on success, -1 if null. The shell calls this
 * AFTER OREFLD'ing the peer sub-cap from its dir_walk result —
 * that ref is still in O1 at this point. */
static int
sup_list_tasks_stash_recipient(void)
{
	int isn;
	asm volatile(
		"omov o14, o1\n"
		"oisn %0, o14"
		: "=r"(isn) : : "r1"
	);
	return isn ? -1 : 0;
}

/* Derive an R+S sub-cap of our reply mailbox (full ref at
 * REPLY_MB_SLOT) into SUP_REPLY_SCRATCH. Same dance as sup_spawn. */
static int
sup_list_tasks_derive_reply_cap(void)
{
	int status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, %1\n"           /* CAP_R | CAP_S = 9 */
		"call  #0x103\n"               /* ObjDerive */
		"nop\n"
		"orefst o1, %2(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_S),
		  "i"(SUP_REPLY_SCRATCH_OFFSET)
		: "r1", "r2", "r4"
	);
	return status;
}

/* Public API. Caller must have OREFLD'd the target supervisor's
 * R+S sub-cap into O1 immediately before calling — the entry asm
 * stashes it to O14. (No clean way to pass an OR ref through a C
 * arg yet; this is the same pattern dir.c uses for dir_register.) */
int
sup_list_tasks(char *dst, int max)
{
	if (sup_list_tasks_stash_recipient() != 0) return -1;

	int status = sup_reply_mailbox_init();
	if (status != 0) return -2;

	status = sup_list_tasks_derive_reply_cap();
	if (status != 0) return -3;

	sup_list_tasks_send();

	int reply_status, length;
	if (sup_list_tasks_recv(&reply_status, &length) != 0) return -4;
	if (reply_status != 0) return reply_status;
	if (length == 0) return 0;

	int copy_len = (length < max) ? length : max;
	if (sup_list_fetch_reply(dst, copy_len) != 0) return -5;
	return copy_len;
}

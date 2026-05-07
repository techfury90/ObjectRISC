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

/* Test whether the supervisor sub-cap was provided at boot.
 * The "0=0@0" pad service-spec produces a literal-zero ref
 * (Phase 45a tweak in simorisc), so OISN works directly. */
static int
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
	int status;

	/* Fallback path: caller wasn't launched by a supervisor (boot
	 * O8 was null → SUP_SLOT is null). Hand off to orx_spawn
	 * directly — same code that orx_run uses. This is the only
	 * exercised path through 45a; the SEND-RPC branch below is
	 * the wire protocol the supervisor will speak in 45b once
	 * boot.sh actually launches supervisor.orx as the leader. */
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

	/* Derive R+S sub-cap from our reply mailbox. */
	asm volatile(
		"orefld o1, 552(o12)\n"        /* full mailbox ref */
		"addiu r4, r0, 9\n"            /* R|S */
		"call  #0x103\n"               /* ObjDerive → O1 */
		"nop\n"
		"omov  o15, o1\n"              /* park in O15 briefly */
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r4"
	);
	if (status != 0) return status;

	/* SEND to the supervisor.
	 *   O1 = supervisor sub-cap (recipient)
	 *   O2 = bytes ref (request payload)
	 *   O3 = reply sub-cap
	 *   R4 = op = 1 (spawn)
	 *   R5 = payload length */
	asm volatile(
		"orefld o1, 544(o12)\n"        /* supervisor sub-cap */
		"omov   o2, o14\n"
		"omov   o3, o15\n"
		"addiu  r4, r0, 1\n"
		"addu   r5, %1, r0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(0), "r"(payload_len)
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

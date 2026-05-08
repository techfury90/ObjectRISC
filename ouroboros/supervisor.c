/*
 * supervisor.c — Ouroboros init / spawn server.
 *
 * Phase 45b — CPU 0's boot leader. The supervisor allocates a
 * spawn-service mailbox, derives a sub-cap into ORX_SLOT_CHILD_O8
 * (so every TaskCreate it does injects the cap into the child's
 * O8 → SUP_SLOT path), TaskCreates the shell as its first user
 * task, and then services spawn requests over SEND. The shell's
 * `run`/`edit` go through libc's sup_spawn → SEND → us → orx_spawn
 * round trip; the shell's `exit` SENDs op=2 (sup_shutdown) so we
 * can wind down the loop without polling.
 *
 * The supervisor:
 *   - holds the boot service refs (term, kbd, grid, hostfsd) that
 *     each spawned task should inherit;
 *   - owns the .orx loader (orx_spawn machinery);
 *   - exposes two ops: 1 = spawn, 2 = shutdown. Future ops slot in
 *     at 3+ (kill/wait/etc.) when we want the supervisor to
 *     mediate those too — for now callers operate on task refs
 *     directly since refs work transparently across local CPU
 *     boundaries.
 *
 * Wire protocol on the spawn mailbox (sender → us, RecvQueuePoll
 * dequeues with R3 carrying the first int arg, O1-O4 the OR
 * payload):
 *     R3 = op (1 = spawn, 2 = shutdown)
 *     R4 = byte length of the request payload (0 for shutdown)
 *     O2 = TAG_DATA bytes object: path\0args\0cwd\0   (spawn only)
 *     O3 = reply_cap (R+S sub-cap of caller's mailbox; spawn only —
 *          shutdown is fire-and-forget)
 *
 * Spawn reply (us → caller, SEND):
 *     R4 = status (0 OK, negative for orx_spawn-style failures)
 *     O2 = the new task ref (or null on failure)
 *
 * OPR map maintained by the supervisor:
 *     O5 = console     (boot)
 *     O6 = keyboard    (boot)
 *     O7 = grid        (boot)
 *     O8 = hf mailbox  (claimed by hf_init; orx_task_create swaps
 *                       to the spawn-service sub-cap during each
 *                       TaskCreate so the child's task_init
 *                       harvests it from O8 → SUP_SLOT)
 *     O9 = supervisor's spawn-service mailbox (full ref, allocated
 *           at boot)
 *     O10 = hostfsd    (boot)
 *     O11..O15 = libc-managed (task_init, etc.)
 *
 * Diagnostics go via firmware ConsoleWrite (`print_str` / `print_int`)
 * to the host's stdout — NOT the Tk terminal. That keeps the
 * supervisor independent of term_init / term_print_only_init, frees
 * O14 for use as scratch around TaskCreate, and matches the
 * convention we settled on for `dhry.c` and friends earlier. NB:
 * print_str reads O3 as the data-section ref, which orx_spawn
 * clobbers — wrap calls in SUP_PRINT() to restore O3 from O15
 * (where task_init parked the boot data ref).
 */

#include "liborisc.h"

#define SHELL_PATH "/programs/shell.orx"

#define TAG_SERVICE 0x4103
#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* Byte offset within O12 of orx_task_create's "child O8 override"
 * slot (Phase 45a). Set this once at boot; orx_task_create swaps
 * it into the child's O8 around every TaskCreate, transparently
 * to the rest of orx_spawn. */
#define ORX_SLOT_CHILD_O8_OFFSET 560

/* Byte offset within O12 of the supervisor's scratch slot — used
 * to stash the per-request reply_cap (sender's O3) across the
 * orx_spawn call, which clobbers O1..O3 via its manifest restore.
 * Allocated as the last slot of the libc OR-store object (see
 * task.c's ORX_STATE_BYTES comment). */
#define SUP_SCRATCH_SLOT_OFFSET  576

/* console_write reads O3 to find the data section. orx_spawn /
 * sup_spawn / ObjDerive all clobber O3 along the way. task_init
 * parked the boot data ref in O15 — restore O3 from O15 before
 * any print_str call that follows a primitive that touches O3.
 *
 * We wrap print_str / print_int via these macros so the rest of
 * the file reads naturally; "sup_" prefix to avoid colliding
 * with the libc symbols. */
static void sup_restore_data_ref(void)
{
	asm volatile("omov o3, o15");
}
#define SUP_PRINT(s)    do { sup_restore_data_ref(); print_str(s); } while (0)
#define SUP_PRINT_INT(n) do { sup_restore_data_ref(); print_int(n); } while (0)

/* Read the firmware PROCID control register (Vol V §2.10, ctrl 7).
 * Phase 45c: every CPU runs the same supervisor.orx; only the
 * leader (PROCID == 0) spawns the shell as its first user task,
 * the rest sit in the dispatch loop ready to service spawn
 * requests from peers (when 45e wires that up). */
static int
read_procid(void)
{
	int pid;
	asm volatile("lctrl %0, $7" : "=r"(pid));
	return pid;
}

/* Allocate a 16-byte TAG_SERVICE object, attach a queue, park the
 * full ref in O9. Subsequent TaskCreates derive a fresh R+S sub-cap
 * from O9 each time and inject it into the child's O8. */
static int
allocate_service_mailbox(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"           /* TAG_SERVICE */
		"addiu r6, r0, %2\n"           /* R|W|S|V|C */
		"call  #0x100\n"               /* ObjAlloc → O1 */
		"nop\n"
		"omov  o9, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* ReceiveQueueAttach(O1=mailbox, R4=depth=8). 8 because we
	 * might queue several spawn requests if multiple shell-spawns
	 * hit us in close sequence; usually it's 1 outstanding. */
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 8\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Derive a fresh R+S sub-cap from our mailbox (O9) and OREFST it
 * into ORX_SLOT_CHILD_O8 so orx_task_create injects it into every
 * subsequent TaskCreate's child-O8. Called once at boot after the
 * mailbox is allocated. */
static void
install_child_o8_override(void)
{
	asm volatile(
		"omov   o1, o9\n"
		"addiu  r4, r0, %0\n"          /* CAP_R | CAP_S = 9 */
		"call   #0x103\n"              /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"orefst o1, 560(o12)\n"        /* ORX_SLOT_CHILD_O8 */
		:
		: "i"(CAP_R | CAP_S)
		: "r1", "r2", "r4"
	);
}

/* Read the spawn request bytes from the sender's TAG_DATA object
 * via byte-load through a temporary mapping, then call
 * supervisor_spawn with the unpacked path/args/cwd.
 *
 * O2 (the bytes ref) is in the dequeued message's OR slot; we map
 * it R-only at SPAWN_REQ_VA, parse, unmap. */
#define SPAWN_REQ_VA      0x00700000
#define SPAWN_REQ_MAX     256

/* Bring O2 (the bytes ref from the dequeued message) up into O1
 * for MapObject, since MapObject reads its target from O1. */
static int
map_spawn_request(int len)
{
	int status;
	asm volatile(
		"omov   o1, o2\n"
		"lui    r4, 0x70\n"            /* va = 0x700000 */
		"addu   r5, r0, r0\n"          /* offset = 0 */
		"addiu  r6, r0, %1\n"          /* prot = R */
		"addu   r7, %2, r0\n"          /* length */
		"call   #0x110\n"
		"nop\n"
		"addu   %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R), "r"(len)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	return status;
}

static int
unmap_spawn_request(int len)
{
	int status;
	asm volatile(
		"lui    r4, 0x70\n"
		"addu   r5, %1, r0\n"
		"call   #0x111\n"
		"nop\n"
		"addu   %0, r2, r0"
		: "=r"(status)
		: "r"(len)
		: "r2", "r3", "r4", "r5"
	);
	return status;
}

/* Copy a NUL-terminated string from *p into dst (capacity cap).
 * Updates *p past the NUL. Returns dst (or empty-string pointer if
 * the source was zero-length). hf_open needs paths in stack or
 * data segments — not in the request-buffer mapping at 0x700000 —
 * so we copy out before unmapping rather than alias into the
 * mapping. */
static void
copy_cstr_from(const char **p, char *dst, int cap)
{
	int n = 0;
	while (**p && n + 1 < cap) {
		dst[n++] = **p;
		(*p)++;
	}
	dst[n] = '\0';
	while (**p) (*p)++;            /* skip remainder if truncated */
	(*p)++;                        /* step past NUL */
}

/* SEND a (status, task_ref) pair back to the requester's reply_cap
 * (held in O3 from the dequeued message). The task ref is in O1
 * (returned by orx_spawn via task_register_o1 — actually we have
 * a task_t handle, so OREFLD it from the table first). */
static void
reply_to_requester(task_t t, int status)
{
	/* Get the task ref into O1, or null if status != 0. */
	if (status == 0 && t >= 0) {
		/* Translate task_t back to its OR ref. The libc task table
		 * holds the ref at slot t × 8. There's no public libc fn for
		 * "get the ref of a task_t" — we OREFLD directly. */
		switch (t) {
		case  0: asm volatile("orefld o1, 0(o12)");   break;
		case  1: asm volatile("orefld o1, 8(o12)");   break;
		case  2: asm volatile("orefld o1, 16(o12)");  break;
		case  3: asm volatile("orefld o1, 24(o12)");  break;
		case  4: asm volatile("orefld o1, 32(o12)");  break;
		case  5: asm volatile("orefld o1, 40(o12)");  break;
		case  6: asm volatile("orefld o1, 48(o12)");  break;
		case  7: asm volatile("orefld o1, 56(o12)");  break;
		case  8: asm volatile("orefld o1, 64(o12)");  break;
		case  9: asm volatile("orefld o1, 72(o12)");  break;
		case 10: asm volatile("orefld o1, 80(o12)");  break;
		case 11: asm volatile("orefld o1, 88(o12)");  break;
		case 12: asm volatile("orefld o1, 96(o12)");  break;
		case 13: asm volatile("orefld o1, 104(o12)"); break;
		case 14: asm volatile("orefld o1, 112(o12)"); break;
		case 15: asm volatile("orefld o1, 120(o12)"); break;
		default: asm volatile("onull o1"); break;
		}
	} else {
		asm volatile("onull o1");
	}

	/* SEND to reply_cap. The cap was stashed into SUP_SCRATCH_SLOT
	 * by handle_spawn_request before the orx_spawn call clobbered
	 * O3. Reload it into O1 (the SEND recipient slot) here.
	 *   recipient = stashed reply_cap
	 *   O2 = task ref (in O1 right now → move to O2 first)
	 *   R4 = status */
	asm volatile(
		"omov   o2, o1\n"              /* O2 = task ref */
		"orefld o1, %1(o12)\n"         /* O1 = stashed reply_cap */
		"onull  o3\n"
		"addu   r4, %0, r0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "r"(status), "i"(SUP_SCRATCH_SLOT_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

/* Service one spawn request that just landed on our queue. The
 * dequeue has filled:
 *   R3 = op       (1 = spawn)
 *   R4 = byte len
 *   O2 = bytes ref
 *   O3 = reply cap
 *
 * IMPORTANT: orx_spawn (called below) clobbers O1..O3 via its
 * manifest restore path. We stash the reply_cap into
 * SUP_SCRATCH_SLOT immediately so reply_to_requester can recover
 * it; the bytes ref in O2 is consumed before the spawn so it
 * doesn't need stashing.
 */
static void
handle_spawn_request(int len)
{
	asm volatile(
		"orefst o3, %0(o12)"
		:
		: "i"(SUP_SCRATCH_SLOT_OFFSET)
	);

	int map_status = map_spawn_request(len);
	if (map_status != 0) {
		reply_to_requester(-1, map_status);
		return;
	}

	/* Synthesize (const char *)SPAWN_REQ_VA via lui+ori — pcc's
	 * literal-cast lowering emits an `la r,N` pseudo and asmorisc
	 * rejects it. Same workaround program_args() uses. */
	const char *p;
	asm volatile(
		"lui  %0, 0x70\n"
		"ori  %0, %0, 0"
		: "=r"(p)
	);

	/* Copy path/args/cwd onto our stack so hf_open / orx machinery
	 * can dispatch them via O11 (stack ref) — they don't know
	 * about the request-buffer mapping. */
	char path[128], args[128], cwd[128];
	copy_cstr_from(&p, path, sizeof(path));
	copy_cstr_from(&p, args, sizeof(args));
	copy_cstr_from(&p, cwd,  sizeof(cwd));

	(void)unmap_spawn_request(len);

	task_t t = orx_spawn(path, args, cwd);
	int status = (t < 0) ? (int)t : 0;

	reply_to_requester(t, status);

	/* Resume the new task — orx_spawn registers but doesn't
	 * resume; we want the task to actually run. */
	if (t >= 0) (void)task_resume(t);
}

/* Dequeue one message from O9. Infinite timeout: the loop is
 * fully event-driven — each spawn request is a SEND, and the shell
 * SENDs an explicit op=2 (shutdown) on `exit` to break us out of
 * the poll. (We can't poll-with-finite-timeout-then-check-shell-
 * state because simorisc only ticks finite timeouts down on the
 * task's current quantum, and a blocked supervisor never gets
 * one once the shell starts running.) */
static int
poll_one_request(int *out_op, int *out_len)
{
	int status, op, len;
	asm volatile(
		"omov  o1, o9\n"               /* mailbox */
		"addiu r4, r0, -1\n"           /* infinite */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0"
		: "=r"(status), "=r"(op), "=r"(len)
		:
		: "r1", "r2", "r3", "r4"
	);
	*out_op = op;
	*out_len = len;
	return status;
}

const char banner_leader[]   = "supervisor: booting (leader)\n";
const char banner_worker[]   = "supervisor: booting (worker)\n";
const char shell_done[]      = "supervisor: shell exited; halting\n";
const char unknown_op[]      = "supervisor: unknown op\n";

int
main(void)
{
	task_init();
	hf_init();
	orx_init();

	if (allocate_service_mailbox() != 0) {
		print_str("supervisor: failed to allocate spawn mailbox\n");
		return 1;
	}

	/* Make every subsequent TaskCreate inherit a fresh sub-cap
	 * of our mailbox in O8. orx_task_create reads
	 * ORX_SLOT_CHILD_O8 and does the swap transparently. */
	install_child_o8_override();

	int procid    = read_procid();
	int is_leader = (procid == 0);

	print_str(is_leader ? banner_leader : banner_worker);

	/* Phase 45c: only the leader (PROCID 0) spawns the shell. Worker
	 * supervisors sit in the dispatch loop ready to service spawn
	 * requests from peers — today nothing SENDs to them, but the
	 * mailbox is allocated, the dispatch loop is running, and the
	 * sub-cap is ready in ORX_SLOT_CHILD_O8 so any task this
	 * supervisor *does* eventually create inherits the right
	 * boot ABI. Phase 45e wires up supervisor-to-supervisor SENDs
	 * for cross-CPU spawn placement. */
	if (is_leader) {
		task_t shell = orx_spawn(SHELL_PATH, "", "/");
		if (shell < 0) {
			SUP_PRINT("supervisor: failed to spawn shell: ");
			SUP_PRINT_INT((int)shell);
			SUP_PRINT("\n");
			return 1;
		}
		if (task_resume(shell) != 0) {
			SUP_PRINT("supervisor: failed to resume shell\n");
			return 1;
		}
		(void)shell;   /* shell-exit detection is via op=2 SEND
		                * below, not task_query. */
	}

	/* Dispatch loop. Wake-up is event-driven: each `run`/`edit`
	 * from a shell SENDs op=1 (spawn), and the leader's shell
	 * SENDs op=2 (sup_shutdown) right before its TaskExit on
	 * `exit`/`quit`. The latter unblocks the leader's poll
	 * deterministically.
	 *
	 * Worker supervisors get torn down externally when the leader
	 * exits — oriscrun's `--leader 0` watches CPU 0 and SIGTERMs
	 * the rest of the process group on its exit. (See
	 * tools/oriscrun.) Without that external teardown a worker
	 * would block in poll_one_request forever waiting for a SEND
	 * that never comes. */
	for (;;) {
		int op, len;
		int status = poll_one_request(&op, &len);
		if (status != 0) continue;

		if (op == 1) {
			handle_spawn_request(len);
		} else if (op == 2 && is_leader) {
			/* Graceful shutdown — sup_shutdown() in libc, called
			 * by the leader's shell right before its TaskExit.
			 * No reply. Workers ignore op=2 (no shell to halt
			 * on); they wait for the external teardown signal. */
			SUP_PRINT(shell_done);
			return 0;
		} else {
			SUP_PRINT(unknown_op);
		}
	}
}

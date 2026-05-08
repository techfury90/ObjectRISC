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

/* Byte offset within O12 of the peer supervisor sub-cap — used by
 * the op=1 relay path when a spawn request specifies target_pid !=
 * self.procid. Phase 45e wires a single peer in; null when the
 * supervisor was launched without a peer (or is the only CPU). */
#define PEER_SUP_SLOT_OFFSET     584

/* Sentinel target_pid passed in R5 of an op=1 SEND meaning "spawn
 * on whatever CPU this supervisor is on" (the historical 45a/b/c
 * behaviour). Any other value < 0xFF is taken as a literal PROCID
 * and triggers the relay path when it doesn't match self.procid. */
#define TARGET_PID_LOCAL         0xFF

/* console_write picks O2 (stack ref) for VAs in [0x1f0000, 0x200000)
 * and O3 (data ref) for VAs in [0x40000, 0x1f0000). Both get
 * clobbered routinely:
 *   - O3 by orx_spawn / sup_spawn / ObjDerive (manifest restore,
 *     etc.)
 *   - O2 by ReceiveQueuePoll's _deliver_queue_msg, which fills
 *     O1..O4 with the dequeued message's OR payload (so after a
 *     poll, O2 holds the request bytes ref, not the boot stack)
 *
 * task_init parks the boot stack in O11 and the boot data in O15;
 * SUP_PRINT_RESTORE puts both back into O2/O3 so print_str (data)
 * AND print_int (which uses a stack-resident buffer) both work.
 *
 * The "sup_" prefix avoids colliding with the libc symbols. */
static void sup_restore_boot_or(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}
#define SUP_PRINT(s)    do { sup_restore_boot_or(); print_str(s); } while (0)
#define SUP_PRINT_INT(n) do { sup_restore_boot_or(); print_int(n); } while (0)

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

/* Maximum spawn-request payload size. Matches sup.c's
 * SPAWN_REQ_BUF_SIZE. Used both as the destination buffer size
 * for ObjFetchBytes and as a sanity cap on incoming `len`. */
#define SPAWN_REQ_MAX     256

/* Phase 45e — replaced map_spawn_request / unmap_spawn_request /
 * copy_cstr_from with a single ObjFetchBytes call. The new
 * primitive (#0x108) copies bytes between two object refs and
 * works transparently for local AND remote sources, so the same
 * code path serves both:
 *   - Direct spawn from a local shell: O2 is a TAG_DATA bytes
 *     object on this CPU; ObjFetchBytes does an in-process bytes
 *     copy.
 *   - Relayed spawn from a peer supervisor: O2 is a remote bytes
 *     object on the originating CPU; ObjFetchBytes issues an
 *     OBJ_READ_REQ over the wire and writes the response into
 *     our local destination.
 *
 * The destination is the boot stack object (O11, parked there by
 * task_init) at the byte offset of a stack-local scratch buffer.
 * No need for a long-lived MapObject — the boot stack is already
 * R+W mapped, so once ObjFetchBytes has filled the buffer we
 * parse it via ordinary VA-based reads.
 *
 * STACK_BOTTOM matches CONTRACT.md §2's default stack layout
 * (STACK_TOP - DEFAULT_STACK_SIZE = 0x200000 - 0x10000). */
#define STACK_BOTTOM      0x001f0000

static int
read_spawn_request(unsigned char *dst, int len)
{
	int status;
	int dst_offset = (int)((unsigned int)dst - STACK_BOTTOM);
	asm volatile(
		"omov  o1, o2\n"            /* source = request bytes ref */
		"omov  o2, o11\n"           /* destination = boot stack */
		"addiu r4, r0, 0\n"         /* src offset */
		"addu  r5, %1, r0\n"        /* dst offset */
		"addu  r6, %2, r0\n"        /* byte count */
		"call  #0x108\n"            /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(dst_offset), "r"(len)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* Copy a NUL-terminated string from *p into dst (capacity cap).
 * Updates *p past the NUL. The source `*p` walks through the
 * stack-resident scratch buffer just filled by ObjFetchBytes;
 * destination is a separate stack array used downstream by
 * hf_open / orx_spawn. */
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

/* Relay an op=1 spawn request to the peer supervisor (held in
 * PEER_SUP_SLOT). The local supervisor doesn't read the bytes —
 * it just forwards the SEND. The bytes ref in O2 stays as-is;
 * the peer's read_spawn_request will OBJ_READ_REQ it across the
 * wire when it actually services the spawn. The reply_cap in O3
 * also stays as-is, so the peer's reply goes directly back to
 * the original requester (we drop out of the response path).
 *
 * R6 in the relayed SEND is set to TARGET_PID_LOCAL so the peer
 * doesn't try to relay again — bounded one-hop topology for now,
 * matches the single-peer PEER_SUP_SLOT model. */
static void
relay_spawn_request(int len)
{
	asm volatile(
		"orefld o1, %0(o12)\n"     /* recipient = peer's mailbox sub-cap */
		"addiu  r4, r0, 1\n"       /* op = spawn */
		"addu   r5, %1, r0\n"      /* forward length */
		"addiu  r6, r0, %2\n"      /* mark as no-further-relay */
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(PEER_SUP_SLOT_OFFSET), "r"(len), "i"(TARGET_PID_LOCAL)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

/* Service one spawn request that just landed on our queue. The
 * dequeue has filled:
 *   R3 = op          (1 = spawn)
 *   R4 = byte len
 *   R5 = target_pid  (TARGET_PID_LOCAL or a literal PROCID; Phase 45e)
 *   O2 = bytes ref
 *   O3 = reply cap
 *
 * If target_pid is set and != self.procid, we relay to the peer
 * (the peer will read bytes, spawn, and reply directly to O3 —
 * we don't enter the response path). Otherwise we spawn locally:
 *
 * IMPORTANT: orx_spawn clobbers O1..O3 via its manifest restore
 * path. We stash the reply_cap into SUP_SCRATCH_SLOT immediately
 * so reply_to_requester can recover it; ObjFetchBytes consumes O2
 * before the spawn so the bytes ref doesn't need stashing.
 */
static void
handle_spawn_request(int len, int target_pid, int self_procid)
{
	if (target_pid != TARGET_PID_LOCAL && target_pid != self_procid) {
		relay_spawn_request(len);
		return;
	}

	asm volatile(
		"orefst o3, %0(o12)"
		:
		: "i"(SUP_SCRATCH_SLOT_OFFSET)
	);

	if (len <= 0 || len > SPAWN_REQ_MAX) {
		reply_to_requester(-1, -1);
		return;
	}

	unsigned char buf[SPAWN_REQ_MAX];
	int fetch_status = read_spawn_request(buf, len);
	if (fetch_status != 0) {
		reply_to_requester(-1, fetch_status);
		return;
	}

	/* Parse buf as path\0args\0cwd\0. Copy onto our stack so
	 * hf_open / orx machinery can dispatch them via O11 (the
	 * boot stack ref). */
	char path[128], args[128], cwd[128];
	const char *p = (const char *)buf;
	copy_cstr_from(&p, path, sizeof(path));
	copy_cstr_from(&p, args, sizeof(args));
	copy_cstr_from(&p, cwd,  sizeof(cwd));

	task_t t = orx_spawn(path, args, cwd);
	int status = (t < 0) ? (int)t : 0;

	reply_to_requester(t, status);

	/* Resume the new task — orx_spawn registers but doesn't
	 * resume; we want the task to actually run. */
	if (t >= 0) (void)task_resume(t);
}

/* Dequeue one message from O9. Infinite timeout: the loop is
 * fully event-driven — each spawn request is a SEND, and the
 * leader's shell SENDs an explicit op=2 (shutdown) on `exit` to
 * break us out of the poll. (We can't poll-with-finite-timeout-
 * then-check-shell-state because simorisc only ticks finite
 * timeouts down on the task's current quantum, and a blocked
 * supervisor never gets one once the shell starts running.)
 *
 * Phase 45e: also returns target_pid (R5 in the dequeued payload,
 * = sender's R6). Sender packs TARGET_PID_LOCAL when it has no
 * preference, or a literal PROCID for explicit `run @N` placement. */
static int
poll_one_request(int *out_op, int *out_len, int *out_target_pid)
{
	int status, op, len, target_pid;
	asm volatile(
		"omov  o1, o9\n"               /* mailbox */
		"addiu r4, r0, -1\n"           /* infinite */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0"
		: "=r"(status), "=r"(op), "=r"(len), "=r"(target_pid)
		:
		: "r1", "r2", "r3", "r4", "r5"
	);
	*out_op = op;
	*out_len = len;
	*out_target_pid = target_pid;
	return status;
}

const char banner_leader[]   = "supervisor: booting (leader)\n";
const char banner_worker[]   = "supervisor: booting (worker)\n";
const char shell_done[]      = "supervisor: shell exited; halting\n";
const char unknown_op[]      = "supervisor: unknown op\n";

int
main(void)
{
	/* Phase 45e: allocate the spawn mailbox FIRST — before
	 * task_init / hf_init / orx_init touch the descriptor table
	 * with their own allocations — so it lands at a deterministic
	 * descriptor index across boots. In socket mode (the boot.sh
	 * configuration) init_cpu reserves 1=code, 2=stack, 3=data,
	 * 4=bootstrap task, then simorisc's populate_self_service
	 * reserves idx 5 (the per-CPU "self" service installed in O4).
	 * The supervisor's first ObjAlloc therefore lands at idx 6.
	 * Static `--service "PEER_PID=6@9"` lines in scripts/boot.sh
	 * synthesize a working sub-cap to any peer supervisor without
	 * runtime discovery. */
	if (allocate_service_mailbox() != 0) {
		print_str("supervisor: failed to allocate spawn mailbox\n");
		return 1;
	}

	task_init();

	/* Harvest boot O8 into PEER_SUP_SLOT BEFORE hf_init runs —
	 * hf_init clobbers O8 with the hostfsd reply mailbox, and
	 * the boot-time peer supervisor sub-cap (wired in by
	 * scripts/boot.sh's `--service "PEER=5@9"` on the O8 slot)
	 * would be lost. task_init already copied O8 to SUP_SLOT on
	 * its way past, so we don't need to also harvest it for the
	 * supervisor's own use — the supervisor doesn't call
	 * sup_spawn — but PEER_SUP_SLOT carries the supervisor-side
	 * peer-relay view, which is a different concern. */
	asm volatile(
		"orefst o8, %0(o12)"
		:
		: "i"(PEER_SUP_SLOT_OFFSET)
	);

	hf_init();
	orx_init();

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
		int op, len, target_pid;
		int status = poll_one_request(&op, &len, &target_pid);
		if (status != 0) continue;

		if (op == 1) {
			handle_spawn_request(len, target_pid, procid);
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

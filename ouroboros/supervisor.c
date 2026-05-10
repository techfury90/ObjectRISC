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

/* Phase 48: the supervisor doesn't spawn /programs/shell.orx
 * directly anymore — each terminal-equipped CPU spawns
 * /programs/login.orx as its first user task instead, and
 * login.orx is the one that brings up shell sessions on demand
 * (welcome banner → press-Enter → spawn shell → on shell exit,
 * loop back).
 *
 * The leader (procid 0) additionally spawns /programs/sysinit.orx
 * BEFORE its login.orx — sysinit installs the /programs MOUNT in
 * oriscdir and exits, having done the singleton system setup the
 * supervisor used to do leader-only inline. */
#define LOGIN_PATH   "/programs/login.orx"
#define SYSINIT_PATH "/programs/sysinit.orx"

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

/* Phase 49: terminal-pass-through. Mirror slots for O5/O6/O7
 * (console/keyboard/grid). The supervisor sets these PER-SPAWN
 * when servicing a relayed request that named a foreign terminal,
 * then orx_task_create swaps them around TaskCreate (same dance
 * as ORX_SLOT_CHILD_O8). We always clear them after orx_spawn so
 * a subsequent local spawn doesn't accidentally inherit a stale
 * override. */
#define ORX_SLOT_CHILD_O5_OFFSET 632
#define ORX_SLOT_CHILD_O6_OFFSET 648
#define ORX_SLOT_CHILD_O7_OFFSET 664

/* Byte offset within O12 of the supervisor's scratch slot — used
 * to stash the per-request reply_cap (sender's O3) across the
 * orx_spawn call, which clobbers O1..O3 via its manifest restore.
 * Allocated as the last slot of the libc OR-store object (see
 * task.c's ORX_STATE_BYTES comment). */
#define SUP_SCRATCH_SLOT_OFFSET  576

/* Byte offset within O12 of the boot-parent slot (the directory
 * mailbox sub-cap on the supervisor's own boot). task.c renames
 * this BOOT_PARENT_SLOT in 45f; mirror the constant locally so the
 * supervisor doesn't need to pull task.c's libc header in. */
#define BOOT_PARENT_SLOT_OFFSET  544

/* Byte offset within O12 of DIR_SLOT — the directory mailbox
 * sub-cap, which dir.c reads on every dir_*() call. Phase 45e's
 * static PEER_SUP_SLOT lived at the same offset; 45f repurposes
 * it: peer discovery is now via dir_walk("/sys/cpu/<N>/supervisor"),
 * so the static-peer slot retires and the offset becomes DIR_SLOT.
 * The supervisor copies BOOT_PARENT_SLOT → DIR_SLOT at boot since
 * the supervisor's own boot O8 IS the directory mailbox. */
#define DIR_SLOT_OFFSET          584

/* Phase 59 / WM γ.3: when a leader successfully wm_init's,
 * wm_new_window's, and wm_bind_surface(CONSOLE)'s, it parks the
 * resulting WM-mediated CONSOLE sub-cap here.  populate_child_term_-
 * slots reads it back for spawns whose terminal index matches the
 * supervisor's own, so children inherit the WM-mediated console
 * (their output gets glyph-rendered into the framebuffer) instead
 * of the direct /sys/term/<idx>/console path.  Null on workers, on
 * leaders without WM mediation, and for spawns targeting a different
 * terminal than the supervisor's own (cross-terminal hot-attach). */
#define WM_LEADER_CONSOLE_SLOT_OFFSET  696

/* Sentinel target_pid values. PROCIDs occupy 0..0xFD; 0xFE / 0xFF
 * are reserved as routing markers.
 *
 * TARGET_PID_LOCAL (0xFF) — "stay on this supervisor's CPU; do not
 *   round-robin." Used (a) by callers that want strict local
 *   placement via sup_spawn_at(SUP_TARGET_LOCAL, ...), and (b) on
 *   the wire by relay_spawn_request to mark a relayed packet as
 *   "spawn here, no further relay" — without that pinning the
 *   receiver could re-relay, ping-ponging the request indefinitely.
 *
 * TARGET_PID_ANY (0xFE) — Phase 51 "round-robin OK." Plain
 *   sup_spawn fills this so the supervisor's pick_next_cpu can
 *   place the spawn on any live CPU. The receiver dir-walks
 *   /sys/term/<requester>/* (from R7's term_hint) and injects the
 *   requester's terminal services into the child's OPRs so output
 *   routes back regardless of host CPU. */
#define TARGET_PID_LOCAL         0xFF
#define TARGET_PID_ANY           0xFE

/* Path-component buffer size for /sys/cpu/<N>/supervisor renders.
 * "/sys/cpu/255/supervisor" is 23 chars + NUL = 24; round up. */
#define PEER_PATH_BUF_SIZE       64

/* Phase 45f relay scratch slots. dir_walk's reply-mailbox poll
 * clobbers O1..O4, so we stash the dequeued bytes-ref (O2) and
 * reply_cap (O3) here before invoking the directory and pull
 * them back to populate the relayed SEND.
 *
 * Mirror task.c's RELAY_SCRATCH region. */
#define RELAY_BYTES_SLOT_OFFSET   592
#define RELAY_REPLY_SLOT_OFFSET   600

/* dir_walk publishes its resolved ref here on return. We OREFLD
 * from this slot rather than relying on O1 being preserved across
 * the function-call boundary. Mirror task.c's DIR_RESULT_SLOT. */
#define DIR_RESULT_SLOT_OFFSET    616

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

/* Append a small decimal integer (0..255) to buf at offset p,
 * returning the new offset. Shared by render_peer_path and
 * render_term_path — both want "/sys/.../<n>/..." path renders. */
static int
append_decimal(int n, char *buf, int p)
{
	if (n >= 100) {
		buf[p++] = '0' + (n / 100);
		buf[p++] = '0' + ((n / 10) % 10);
		buf[p++] = '0' + (n % 10);
	} else if (n >= 10) {
		buf[p++] = '0' + (n / 10);
		buf[p++] = '0' + (n % 10);
	} else {
		buf[p++] = '0' + n;
	}
	return p;
}

/* Render a fixed-format peer-supervisor path into the supplied
 * buffer: "/sys/cpu/<n>/supervisor". Returns the byte length
 * (excluding the NUL terminator) which the caller passes to
 * dir_walk via strlen-equivalent — but since we know the layout
 * we just compute it here. n must be 0..255. */
static int
render_peer_path(int n, char *buf)
{
	const char prefix[] = "/sys/cpu/";
	const char suffix[] = "/supervisor";
	int i, p = 0;
	for (i = 0; prefix[i]; i++) buf[p++] = prefix[i];
	p = append_decimal(n, buf, p);
	for (i = 0; suffix[i]; i++) buf[p++] = suffix[i];
	buf[p] = '\0';
	return p;
}

/* Render "/sys/term/<n>/" + suffix into buf — used by the per-CPU
 * terminal-registration block in main() to publish each CPU's
 * boot O5/O6/O7 under its own procid. Returns byte length. */
static int
render_term_path(int n, const char *suffix, char *buf)
{
	const char prefix[] = "/sys/term/";
	int i, p = 0;
	for (i = 0; prefix[i]; i++) buf[p++] = prefix[i];
	p = append_decimal(n, buf, p);
	buf[p++] = '/';
	for (i = 0; suffix[i]; i++) buf[p++] = suffix[i];
	buf[p] = '\0';
	return p;
}

/* Phase 47: walk a path in oriscdir; if it resolves to a LEAF, the
 * resolved ref is in DIR_RESULT_SLOT (offset 616) per dir_walk's
 * contract. Returns 0 on a successful LEAF resolution, -6 when no
 * directory is wired (caller should keep the existing OPR), or
 * other negative codes on real errors.
 *
 * Retries briefly on -2 (NOT_FOUND): device daemons self-register
 * at startup but the registration packet may still be in flight
 * when the supervisor's first walk lands. Each retry is naturally
 * spaced by the wire round-trip of the failed walk (~ms scale),
 * so 5 attempts cover any reasonable launch-order race without
 * stalling boot when the path won't ever exist (e.g. workers
 * with no terminal in test_supervisor_run_at). NO_DIRECTORY (-6)
 * doesn't retry — the absence of an oriscdir is structural.
 *
 * The CALLER follows up with `orefld oN, 616(o12)` to load the
 * resolved ref into the target OPR — we can't index OPRs at
 * runtime, so per-target OREFLDs live in main() right after each
 * call. */
static int
sup_walk_for_opr(const char *path)
{
	int kind;
	char rem[16];          /* unused for LEAFs, just satisfies the API */
	int attempt;
	for (attempt = 0; attempt < 5; attempt++) {
		int rc = dir_walk(path, &kind, rem, sizeof(rem));
		if (rc == -6)               return -6;       /* no directory */
		if (rc == -2) {                              /* not found yet */
			task_yield();           /* let other CPUs / devices run */
			continue;
		}
		if (rc < 0)                 return rc;       /* real error */
		if (kind != DIR_KIND_LEAF)  return -1;
		return 0;
	}
	return -2;     /* timed out waiting for registration */
}

/* Relay an op=1 spawn request to the peer supervisor for
 * `target_pid`. Phase 45f: we look up the peer's spawn-mailbox
 * sub-cap via dir_walk("/sys/cpu/<N>/supervisor") rather than
 * relying on a static --service slot.
 *
 * Sequence:
 *   1. Stash O2 (bytes ref) and O3 (reply_cap) into scratch slots
 *      because dir_walk's queue-poll clobbers O1..O4.
 *   2. Render the peer path and dir_walk it. On LEAF success,
 *      O1 holds the peer mailbox sub-cap.
 *   3. Restore O2 and O3 from the scratch slots.
 *   4. SEND the relayed spawn request to O1, with R6 =
 *      TARGET_PID_LOCAL so the peer doesn't relay again. The
 *      reply_cap rides along unchanged so the peer's eventual
 *      response goes straight back to the original requester. */
static int
relay_spawn_request(int len, int target_pid, int term_hint_plus_one)
{
	/* 1. Stash O2 and O3. */
	asm volatile(
		"orefst o2, %0(o12)\n"
		"orefst o3, %1(o12)"
		:
		: "i"(RELAY_BYTES_SLOT_OFFSET), "i"(RELAY_REPLY_SLOT_OFFSET)
	);

	/* 2. Resolve the peer. */
	char path[PEER_PATH_BUF_SIZE];
	render_peer_path(target_pid, path);
	int kind;
	char remainder[16];
	int rc = dir_walk(path, &kind, remainder, sizeof(remainder));
	if (rc < 0 || kind != DIR_KIND_LEAF) {
		/* No peer registered for this PROCID. Restore O3 for the
		 * error reply and bail. */
		asm volatile(
			"orefld o3, %0(o12)"
			: : "i"(RELAY_REPLY_SLOT_OFFSET) : "r1"
		);
		return -1;
	}

	/* 3. Restore O2 (bytes) and O3 (reply_cap), and load O1 from
	 * DIR_RESULT_SLOT (where dir_walk parked the resolved peer
	 * mailbox sub-cap). Three OREFLDs in a single asm so pcc
	 * can't intersperse anything that touches OPRs. */
	asm volatile(
		"orefld o2, %0(o12)\n"
		"orefld o3, %1(o12)\n"
		"orefld o1, %2(o12)"
		:
		: "i"(RELAY_BYTES_SLOT_OFFSET),
		  "i"(RELAY_REPLY_SLOT_OFFSET),
		  "i"(DIR_RESULT_SLOT_OFFSET)
	);

	/* 4. SEND.
	 *
	 * R7 carries terminal_hint+1 (Phase 49). Encoding: 0 = "no
	 * terminal," N+1 = "child should run with terminal index N's
	 * console/keyboard/grid" — receiver dir_walks /sys/term/<N>/*
	 * and injects via ORX_SLOT_CHILD_O5/O6/O7. The +1 bias keeps a
	 * sender that doesn't know about Phase 49 (R7=0) from accidentally
	 * naming terminal 0 as the override. */
	asm volatile(
		"addiu  r4, r0, 1\n"        /* op = spawn */
		"addu   r5, %0, r0\n"       /* forward length */
		"addiu  r6, r0, %1\n"       /* mark as no-further-relay */
		"addu   r7, %2, r0\n"       /* terminal hint (+1 biased) */
		"send   o1\n"
		:
		: "r"(len), "i"(TARGET_PID_LOCAL), "r"(term_hint_plus_one)
		: "r4", "r5", "r6", "r7"
	);
	return 0;
}

/* Phase 48: forward an op=2 (shutdown) SEND to the leader supervisor.
 * `exit`/`quit` from a worker's shell wakes its OWN supervisor with
 * op=2, which would otherwise just halt the worker — the leader (and
 * thus oriscrun's --leader watchdog) keeps running, leaving the
 * simulator alive after the user asked it to shut down. By relaying
 * op=2 to /sys/cpu/0/supervisor, the leader processes its own op=2
 * cascade, exits CPU 0, and oriscrun tears down the workers via
 * SIGTERM. (We still cascade-kill our own children before halting,
 * keeping the worker-side teardown deterministic regardless of the
 * leader's response time.) Returns 0 on success, negative on dir_walk
 * failure (no leader registered — exotic, e.g. single-CPU workers
 * built for a different scenario). */
static int
relay_shutdown_to_leader(void)
{
	char path[PEER_PATH_BUF_SIZE];
	render_peer_path(0, path);
	int kind;
	char remainder[16];
	int rc = dir_walk(path, &kind, remainder, sizeof(remainder));
	if (rc < 0 || kind != DIR_KIND_LEAF)
		return rc < 0 ? rc : -1;

	/* dir_walk parked the leader's spawn-mailbox sub-cap into
	 * DIR_RESULT_SLOT. Load it into O1 and SEND op=2 with no
	 * payload — same shape as sup_shutdown's wire op. */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addiu  r4, r0, 2\n"        /* op = shutdown */
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(DIR_RESULT_SLOT_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);
	return 0;
}

/* Phase 51: round-robin counter, one per supervisor. Initialised in
 * main() to (procid + 1) so the first relay from this supervisor
 * lands on the next live CPU (typically a peer, not self), giving
 * an even initial spread on a multi-CPU boot. The pick_next_cpu
 * helper iterates through live /sys/cpu/<N>/supervisor entries
 * starting from this counter and advances. Per-supervisor (no shared
 * state); under steady load with N supervisors the global
 * distribution stays roughly fair.
 *
 * Round-robin is gated on `term_hint > 0` in handle_spawn_request: a
 * Phase-51-aware sup_spawn always packs its terminal_idx + 1 into R7,
 * so we can safely round-robin and rely on the receiver dir-walking
 * /sys/term/<N>/* to inject the requester's terminal into the
 * child's OPRs. (R7 == 0 → no info → stay local; preserves the
 * Phase-49 contract.) */
#define MAX_PROCID 16
static int next_cpu_counter;

/* Phase 52: per-task name stash for `ps`. Indexed by libc task_t,
 * stores the basename of the .orx path each spawn loaded (e.g.
 * "shell.orx" out of "/programs/shell.orx"). Used by the op=5
 * SUP_OP_LIST_TASKS handler to render human-readable task lists.
 *
 * 24 bytes per slot is enough for typical names ("shell.orx",
 * "session_manager.orx") with NUL room. Truncation is silent. */
#define TASK_NAME_MAX 24
#define TASK_NAME_SLOTS 16
static char task_names[TASK_NAME_SLOTS * TASK_NAME_MAX];

/* Find the last '/' in `path` and return the byte after it (i.e.,
 * basename). For "/programs/shell.orx" returns "shell.orx"; for
 * "shell.orx" or "" returns the input unchanged. Pure pointer math —
 * no copy. */
static const char *
basename_of(const char *path)
{
	const char *p = path;
	const char *last = path;
	while (*p) {
		if (*p == '/') last = p + 1;
		p++;
	}
	return last;
}

/* Stash the basename of `path` into task_names[t]. Caller passes the
 * libc task_t handle returned by orx_spawn. Overwrites any previous
 * entry — fine because supervisor task slots aren't reused while the
 * task is live, and a slot reused after task_free naturally needs a
 * fresh name. */
static void
stash_task_name(int t, const char *path)
{
	if (t < 0 || t >= TASK_NAME_SLOTS) return;
	const char *base = basename_of(path);
	char *dst = &task_names[t * TASK_NAME_MAX];
	int i = 0;
	while (i + 1 < TASK_NAME_MAX && base[i]) {
		dst[i] = base[i];
		i++;
	}
	dst[i] = '\0';
}

/* Wrapper: orx_spawn + name stash. Use this everywhere the supervisor
 * loads a child .orx so `ps` can label them. */
static task_t
sup_spawn_named(const char *path, const char *args, const char *cwd)
{
	task_t t = orx_spawn(path, args, cwd);
	if (t >= 0) stash_task_name(t, path);
	return t;
}

static int
pick_next_cpu(int self_procid)
{
	int i;
	/* Walk candidates in round-robin order. Self IS a valid pick:
	 * with no peers registered yet (single-CPU boot, or boot-time
	 * races) we land on self every time and stay local — same as
	 * the no-round-robin path. With peers, the counter spreads
	 * picks evenly: e.g. with 2 CPUs and counter biased to self+1,
	 * the sequence is peer, self, peer, self, ... a clean
	 * alternation. */
	for (i = 0; i < MAX_PROCID; i++) {
		int candidate = (next_cpu_counter + i) % MAX_PROCID;
		char path[PEER_PATH_BUF_SIZE];
		render_peer_path(candidate, path);
		if (sup_walk_for_opr(path) == 0) {
			next_cpu_counter = candidate + 1;
			return candidate;
		}
	}
	/* No supervisor registered anywhere — shouldn't happen (we
	 * dir_register'd ourselves at boot) but fall back to self. */
	next_cpu_counter = self_procid + 1;
	return self_procid;
}

/* OISN-style probe of WM_LEADER_CONSOLE_SLOT.  Returns 1 if null. */
static int
wm_leader_console_isn(void)
{
	int isn;
	asm volatile(
		"orefld o14, %1(o12)\n"
		"oisn   %0, o14"
		: "=r"(isn)
		: "i"(WM_LEADER_CONSOLE_SLOT_OFFSET)
		: "r14"
	);
	return isn;
}

/* Phase 49: populate ORX_SLOT_CHILD_O5/O6/O7 from
 * /sys/term/<term_idx>/{console,keyboard,grid}. orx_task_create
 * inside the upcoming orx_spawn swaps these into the child's OPR
 * file just before TaskCreate. After the spawn returns, the caller
 * MUST clear these slots (clear_child_term_slots) so a subsequent
 * local spawn doesn't accidentally inherit a stale terminal.
 *
 * Phase 59 / WM γ.3: for the CONSOLE slot specifically, prefer the
 * leader's WM-mediated CONSOLE sub-cap (parked at WM_LEADER_CONSOLE_-
 * SLOT during boot) when `term_idx` matches the supervisor's own
 * terminal.  That routes children through the WM so their output
 * gets glyph-rendered into the framebuffer.  Cross-terminal
 * hot-attach (term_idx != my_terminal_idx) and non-WM workers fall
 * through to the direct /sys/term/<N>/console walk.
 *
 * Best-effort: missing services (a partial /sys/term/<N> subtree)
 * leave the corresponding slot null and the child inherits this
 * supervisor's own boot OPR for that service. The supervisor's
 * boot O5/O6/O7 are wired to ITS terminal, which is the wrong one
 * — but it's better than null for programs that never use the
 * service in question (e.g., a CPU-bound background task). */
static void
populate_child_term_slots(int term_idx)
{
	char path[PEER_PATH_BUF_SIZE];

	int wired_console = 0;
	if (term_idx == task_my_terminal_idx()
	    && wm_leader_console_isn() == 0) {
		/* WM-mediated CONSOLE for the leader's own terminal. */
		asm volatile(
			"orefld o14, %0(o12)\n"
			"orefst o14, %1(o12)"
			:
			: "i"(WM_LEADER_CONSOLE_SLOT_OFFSET),
			  "i"(ORX_SLOT_CHILD_O5_OFFSET)
			: "r14"
		);
		wired_console = 1;
	}
	if (!wired_console) {
		render_term_path(term_idx, "console", path);
		if (sup_walk_for_opr(path) == 0) {
			asm volatile(
				"orefld o14, %0(o12)\n"
				"orefst o14, %1(o12)"
				:
				: "i"(DIR_RESULT_SLOT_OFFSET),
				  "i"(ORX_SLOT_CHILD_O5_OFFSET)
			);
		}
	}
	render_term_path(term_idx, "keyboard", path);
	if (sup_walk_for_opr(path) == 0) {
		asm volatile(
			"orefld o14, %0(o12)\n"
			"orefst o14, %1(o12)"
			:
			: "i"(DIR_RESULT_SLOT_OFFSET),
			  "i"(ORX_SLOT_CHILD_O6_OFFSET)
		);
	}
	render_term_path(term_idx, "grid", path);
	if (sup_walk_for_opr(path) == 0) {
		asm volatile(
			"orefld o14, %0(o12)\n"
			"orefst o14, %1(o12)"
			:
			: "i"(DIR_RESULT_SLOT_OFFSET),
			  "i"(ORX_SLOT_CHILD_O7_OFFSET)
		);
	}
}

static void
clear_child_term_slots(void)
{
	asm volatile(
		"orefst o0, %0(o12)\n"
		"orefst o0, %1(o12)\n"
		"orefst o0, %2(o12)"
		:
		: "i"(ORX_SLOT_CHILD_O5_OFFSET),
		  "i"(ORX_SLOT_CHILD_O6_OFFSET),
		  "i"(ORX_SLOT_CHILD_O7_OFFSET)
	);
}

/* --- Phase 52: hot-attach for terminals ----------------------------
 *
 * The leader supervisor's dispatch loop wakes periodically (finite-
 * timeout poll, see poll_one_request_timed) and scans /sys/term for
 * any newly-registered terminal directories. For each one not yet
 * seen, it spawns a fresh login.orx with the appropriate Phase 49
 * terminal-pass-through so the spawned login binds to /sys/term/
 * <new-idx>/{console,keyboard,grid} regardless of which CPU it
 * lands on (round-robin via SUP_TARGET_ANY in plain orx_spawn —
 * actually we use orx_spawn local since hot-attach happens on the
 * leader's CPU; the child still gets the right terminal services
 * via populate_child_term_slots).
 *
 * Why baked into the supervisor instead of a separate session_
 * manager.orx program: an earlier prototype put session_manager in
 * its own .orx, but loading that ~30 KiB file via hf_read on cpu0's
 * boot path widens the leader's startup window enough that a fast
 * peer worker shell can finish its session and relay op=2 BEFORE
 * cpu0's own login has even rendered the welcome banner — and the
 * cascade-kill curtails the leader's shell before it really starts.
 * test_multiterminal demonstrated the regression conclusively.
 * Embedding the hot-attach logic here avoids the .orx load
 * entirely; the supervisor is already loaded.
 *
 * Boot seeding: we mark all terminals registered AT BOOT TIME as
 * already-seen, so the per-supervisor has_terminal block (which
 * spawns a boot login for each CPU's own terminal) doesn't get
 * doubled up by the first hot-attach scan. From then on, only
 * NEWLY-appearing terminals trigger spawns. */

#define HOT_ATTACH_MAX_TERMS 16
#define HOT_ATTACH_LIST_BUF  256
static unsigned int hot_attach_seen;

/* Phase 54: tracking for kill-on-detach. terminal_login_task[idx]
 * carries the task_t of the login bound to terminal index idx, or
 * -1 if none. Populated when a login is spawned (either by the
 * has_terminal boot block or by hot_attach_maybe_spawn), consulted
 * when the periodic scan notices /sys/term/<idx> has disappeared
 * — at which point we task_kill the bound login so it doesn't
 * loop forever on ESTALE keyboard reads. */
static task_t terminal_login_task[HOT_ATTACH_MAX_TERMS];

/* Set by hot_attach_collect during the scan's first pass; consumed
 * by hot_attach_scan in its second-pass diff. */
static unsigned int hot_attach_present_mask;

typedef void (*visit_fn)(int idx);

/* Parse a leading run of decimal digits in `s` into `*out`. Returns
 * the number of digits consumed; 0 = no digits. Stops at any non-
 * digit (oriscdir suffixes directory names with '/', so "0/" parses
 * as 0 and we stop at the slash). */
static int
hot_attach_parse_decimal(const char *s, int *out)
{
	int v = 0, n = 0;
	while (s[n] >= '0' && s[n] <= '9') {
		v = v * 10 + (s[n] - '0');
		n++;
	}
	if (n == 0) return 0;
	*out = v;
	return n;
}

/* dir_list returns the entry count; the byte length of the NUL-
 * separated names buffer needs to be derived. We trust the count
 * and walk forward, stopping after `count` NUL terminators. */
static int
hot_attach_listing_byte_len(const char *buf, int cap, int count)
{
	int i = 0, found = 0;
	while (i < cap && found < count) {
		while (i < cap && buf[i] != '\0') i++;
		if (i < cap) {
			i++;
			found++;
		}
	}
	return i;
}

/* Mark `idx` as seen without spawning. Used at boot to seed
 * hot_attach_seen with the terminals the per-supervisor has_terminal
 * block already handled. */
static void
hot_attach_mark(int idx)
{
	if (idx >= 0 && idx < HOT_ATTACH_MAX_TERMS)
		hot_attach_seen |= (1u << idx);
}

/* Spawn login for `idx` if not already seen, then mark it seen. */
static void
hot_attach_maybe_spawn(int idx)
{
	if (idx < 0 || idx >= HOT_ATTACH_MAX_TERMS) return;
	if (hot_attach_seen & (1u << idx)) return;
	hot_attach_seen |= (1u << idx);

	/* Phase 49 pass-through dance: fill ORX_SLOT_CHILD_O5/O6/O7
	 * from /sys/term/<idx>/* so the spawned login wakes up bound
	 * to the right terminal services. orx_set_child_terminal_idx
	 * also stuffs idx+1 into R5 → child's R4 → _orisc_init_r4 so
	 * its libc task_init reads back my_terminal_idx = idx (for
	 * any future sup_spawn from that login carrying the right R7
	 * routing hint). */
	populate_child_term_slots(idx);
	orx_set_child_terminal_idx(idx);
	task_t t = sup_spawn_named(LOGIN_PATH, "", "/");
	clear_child_term_slots();
	orx_clear_child_terminal_idx();

	if (t < 0) {
		SUP_PRINT("supervisor: hot-attach spawn failed for term=");
		SUP_PRINT_INT(idx);
		SUP_PRINT(" rc=");
		SUP_PRINT_INT((int)t);
		SUP_PRINT("\n");
		return;
	}
	(void)task_resume(t);
	task_yield();    /* same race-fix discipline as handle_spawn_request */
	terminal_login_task[idx] = t;     /* Phase 54: record for kill-on-detach */
	SUP_PRINT("supervisor: hot-attached login for term=");
	SUP_PRINT_INT(idx);
	SUP_PRINT("\n");
}

/* Phase 54: a previously-seen terminal index is no longer present
 * in /sys/term — its oriscterm exited and the directory entries
 * went away. The bound login is still alive but its O5/O6/O7
 * subcaps point at a dead service; its first term_getkey will
 * return ESTALE and login's error path will spin. task_kill
 * deterministically reaps it now. (The slot itself gets reclaimed
 * by reap_exited_tasks on the next pass.) */
static void
hot_attach_detach(int idx)
{
	if (idx < 0 || idx >= HOT_ATTACH_MAX_TERMS) return;
	task_t t = terminal_login_task[idx];
	if (t >= 0) {
		(void)task_kill(t, 137);
		terminal_login_task[idx] = -1;
	}
	hot_attach_seen &= ~(1u << idx);
	SUP_PRINT("supervisor: detached login for term=");
	SUP_PRINT_INT(idx);
	SUP_PRINT("\n");
}

/* First-pass collector for hot_attach_scan: record everything
 * currently present in /sys/term. */
static void
hot_attach_collect(int idx)
{
	if (idx >= 0 && idx < HOT_ATTACH_MAX_TERMS)
		hot_attach_present_mask |= (1u << idx);
}

/* Two-pass scan: walk /sys/term once into present_mask, then diff
 * against hot_attach_seen and act on each transition.
 *   present  + seen     → still alive, no-op
 *   present  + !seen    → newly attached, spawn login
 *   !present + seen     → detached, kill login
 *   !present + !seen    → never was, no-op */
static void
hot_attach_scan(void)
{
	hot_attach_present_mask = 0;
	hot_attach_walk(hot_attach_collect);

	int t;
	for (t = 0; t < HOT_ATTACH_MAX_TERMS; t++) {
		unsigned int bit = 1u << t;
		int present = (hot_attach_present_mask & bit) != 0;
		int seen    = (hot_attach_seen         & bit) != 0;
		if (present && !seen)       hot_attach_maybe_spawn(t);
		else if (!present && seen)  hot_attach_detach(t);
	}
}

/* dir_list /sys/term, walk the names, invoke `visit(idx)` for each
 * integer-named entry. Used by both seed and scan. */
static void
hot_attach_walk(visit_fn visit)
{
	char buf[HOT_ATTACH_LIST_BUF];
	int count = dir_list("/sys/term", buf, sizeof(buf));
	if (count <= 0) return;
	int total = hot_attach_listing_byte_len(buf, sizeof(buf), count);
	int i = 0;
	while (i < total) {
		int idx;
		int n = hot_attach_parse_decimal(buf + i, &idx);
		if (n > 0) visit(idx);
		while (i < total && buf[i] != '\0') i++;
		if (i < total) i++;
	}
}

/* --- Phase 54: slot-table reaping ----------------------------------
 *
 * Every spawn the supervisor performs allocates a slot in its libc
 * task table (16 slots total — see TASK_MAX_CONCURRENT in task.c).
 * task_register_o1 finds the lowest-numbered free slot. Without
 * reaping, EXITED tasks stay in their slots forever; after enough
 * sessions / hot-attach cycles / `run` invocations, the table fills
 * and orx_spawn returns -6 ("task table is full").
 *
 * shell.c has its own per-shell reap_exited_tasks (called once per
 * prompt iteration); the supervisor needs the same hygiene. We call
 * this from two places:
 *
 *   1. The top of handle_spawn_request — natural moment, since
 *      the next thing we'll do is allocate a slot. Failing alloc
 *      due to leaked EXITED slots would cascade into a confused
 *      "spawn failed -6" reply to the shell.
 *
 *   2. Right before each hot_attach_walk pass — periodic backstop.
 *      Even if no spawn requests come in, the leader's hot-attach
 *      logic itself spawns logins for newly-attached terminals,
 *      and those logins eventually exit (e.g., on `logout`).
 *
 * orx_unload internally task_waits (no-op for already-exited),
 * ObjFreeDeferreds the manifest entries, and task_frees the slot.
 * It swallows EFAULT on null-manifest entries (non-orx-spawn'd
 * children, like our own task_init register) so it's safe to call
 * on any EXITED slot. We also clear the per-task name stash so a
 * subsequent `ps` shows the slot as empty rather than carrying a
 * stale "shell.orx (exit 0)" line forever. */
static void
reap_exited_tasks(void)
{
	unsigned int mask = task_active_mask();
	int t;
	for (t = 0; t < TASK_NAME_SLOTS; t++) {
		if (!(mask & (1u << t))) continue;
		struct task_info info;
		if (task_query((task_t)t, &info) != 0) continue;
		if (info.state != TASK_STATE_EXITED) continue;
		(void)orx_unload((task_t)t);
		task_names[t * TASK_NAME_MAX] = '\0';
	}
}


/* --- Phase 52: SUP_OP_LIST_TASKS (op=5) ----------------------------
 *
 * Cross-CPU `ps`. The shell SENDs op=5 to each peer supervisor's
 * mailbox (discovered via dir_walk("/sys/cpu/<N>/supervisor")) and
 * each replies with a TAG_DATA bytes object holding its task list,
 * one task per line. The shell MapObject's R-only, prints, and
 * Unmaps.
 *
 * Wire reply format (this supervisor → shell):
 *     R4 = status   (0 OK; negative on alloc/map failure)
 *     R5 = length   (byte count of valid data in the bytes object)
 *     O2 = bytes ref (TAG_DATA, R+V — null on error)
 *
 * Per-line text format (one task per line, NUL not included; the
 * total length is sent in R5 since clients can't strlen across an
 * MapObject boundary safely):
 *     "[N] STATE NAME\n"          for live tasks
 *     "[N] exited NAME (exit C)\n" for EXITED tasks
 * Slot N is the supervisor's libc task_t handle. NAME comes from
 * task_names[t] (basename stashed at spawn time). Empty list (no
 * live tasks) replies length=0.
 *
 * The shell adds a "CPU N:" header when printing. */

#define SUP_OP_LIST_TASKS  5
#define SUP_OP_DIR_NOTIFY  6   /* Phase 54: oriscdir SENDs this when
                                * /sys/term mutates; supervisor reacts
                                * by re-running the hot-attach scan. */
#define LIST_REPLY_VA      0x00600000   /* matches sup_pack_request's
                                         * scratch VA — only one
                                         * MapObject lives there at a
                                         * time and we Unmap before
                                         * returning. */
#define LIST_BUF_MAX       1024         /* 16 tasks × ~50 chars + slack */

/* Map TASK_STATE_* to a short word printable into our reply buffer.
 * Mirror of shell.c's task_state_label, but inlined here so the
 * supervisor doesn't need to pull in shell internals. */
static const char *
state_word(int state)
{
	switch (state) {
	case TASK_STATE_NEW:       return "new";
	case TASK_STATE_RUNNABLE:  return "runnable";
	case TASK_STATE_RUNNING:   return "running";
	case TASK_STATE_SUSPENDED: return "suspended";
	case TASK_STATE_BLOCKED:   return "blocked";
	case TASK_STATE_EXITED:    return "exited";
	}
	return "?";
}

/* The reply text is written into the static `list_buf`. `list_pos`
 * tracks the write cursor. Using globals (instead of passing the
 * buffer pointer through args) keeps each render helper to ≤2
 * arguments — pcc-orisc's calling-convention lowering chokes on
 * 5-arg static helpers (`adrput: illegal op 57`), seen first in
 * Phase 51 and again here. The trade-off is that the renderers
 * are not re-entrant, but the supervisor's dispatch loop is
 * single-threaded so that's fine.
 *
 * Forward-declare list_buf here; the actual `static char[]`
 * definition lives just before handle_list_tasks_request along
 * with build_task_listing (its primary consumer). */
#define LIST_BUF_MAX       1024
static char list_buf[LIST_BUF_MAX];
static int  list_pos;

/* Append `s` to list_buf, capped at LIST_BUF_MAX-1. */
static void
list_emit_str(const char *s)
{
	while (*s && list_pos + 1 < LIST_BUF_MAX) {
		list_buf[list_pos++] = *s++;
	}
}

static void
list_emit_char(char c)
{
	if (list_pos + 1 < LIST_BUF_MAX) {
		list_buf[list_pos++] = c;
	}
}

/* Append decimal `n` (0..255). Reuses the existing append_decimal
 * helper from supervisor.c (used by render_peer_path / render_term_
 * path). */
static void
list_emit_int(int n)
{
	if (list_pos + 4 > LIST_BUF_MAX) return;
	list_pos = append_decimal(n, list_buf, list_pos);
}

/* Render one task line at list_buf[list_pos]. Format:
 *     "[N] STATE NAME\n"          for live tasks
 *     "[N] exited NAME (exit C)\n" for EXITED tasks */
static void
render_task_line(int slot, const struct task_info *info)
{
	list_emit_char('[');
	list_emit_int(slot);
	list_emit_str("] ");
	list_emit_str(state_word(info->state));
	list_emit_char(' ');
	const char *name = &task_names[slot * TASK_NAME_MAX];
	if (*name) {
		list_emit_str(name);
	} else {
		list_emit_str("(unnamed)");
	}
	if (info->state == TASK_STATE_EXITED) {
		list_emit_str(" (exit ");
		list_emit_int(info->exit_code);
		list_emit_char(')');
	}
	list_emit_char('\n');
}

/* --- handle_list_tasks_request helpers --------------------------- *
 *
 * Split into small functions because pcc-orisc chokes on a single
 * function with multiple asm blocks, register-input asm operands,
 * and several local vars (`adrput: illegal op 57`). Each helper
 * uses at most one asm block + one or two locals. */

/* ObjAlloc(size, TAG_DATA, R+W+V+C) → O1; copy O1 to O14 so it
 * survives subsequent OPR clobbers. Returns firmware status. */
static int
list_alloc_reply(int size)
{
	int status;
	asm volatile(
		"addu  r4, %1, r0\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x100\n"
		"nop\n"
		"omov  o14, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(size), "i"(0x4102),
		  "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* MapObject(O14, LIST_REPLY_VA, 0, R+W, size). */
static int
list_map_reply(int size)
{
	int status;
	asm volatile(
		"omov  o1, o14\n"
		"lui   r4, %1\n"
		"addu  r5, r0, r0\n"
		"addiu r6, r0, %2\n"
		"addu  r7, %3, r0\n"
		"call  #0x110\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(LIST_REPLY_VA >> 16),
		  "i"(CAP_R | CAP_W),
		  "r"(size)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	return status;
}

/* Unmap(LIST_REPLY_VA, size). */
static int
list_unmap_reply(int size)
{
	int status;
	asm volatile(
		"lui   r4, %1\n"
		"addu  r5, %2, r0\n"
		"call  #0x111\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(LIST_REPLY_VA >> 16), "r"(size)
		: "r2", "r3", "r4", "r5"
	);
	return status;
}

/* SEND the reply: O1 = stashed reply_cap, O2 = O14 (bytes ref or
 * null on failure), R4 = status, R5 = length. */
static void
list_send_reply(int reply_status, int length)
{
	asm volatile(
		"orefld o1, %0(o12)\n"
		"omov   o2, o14\n"
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(SUP_SCRATCH_SLOT_OFFSET),
		  "r"(reply_status), "r"(length)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

/* ObjFreeDeferred(O14, 1500ms). */
static void
list_free_reply(void)
{
	asm volatile(
		"omov  o1, o14\n"
		"addiu r4, r0, 1500\n"
		"call  #0x107\n"
		"nop"
		: : : "r2", "r3", "r4"
	);
}

/* Stash the dequeued reply_cap (O3) into SUP_SCRATCH_SLOT so it
 * survives ObjAlloc/MapObject's OPR clobbers. */
static void
list_stash_reply_cap(void)
{
	asm volatile(
		"orefst o3, %0(o12)"
		: : "i"(SUP_SCRATCH_SLOT_OFFSET)
	);
}

/* Walk the libc task table, format one line per live slot into the
 * static reply buffer. Resets list_pos at entry. Returns the byte
 * length of the formatted text. */
static int
build_task_listing(void)
{
	list_pos = 0;
	unsigned int mask = task_active_mask();
	int t;
	for (t = 0; t < TASK_NAME_SLOTS; t++) {
		if (!(mask & (1u << t))) continue;
		struct task_info info;
		if (task_query((task_t)t, &info) != 0) continue;
		render_task_line(t, &info);
	}
	return list_pos;
}

/* Copy `n` bytes from list_buf into the destination at `va`. We pass
 * `va` as an argument rather than referencing LIST_REPLY_VA directly
 * inside the body — pcc-orisc lowers `(char *)CONSTANT_LITERAL` as
 * `la r, CONSTANT` and asmorisc's `la` only accepts labels, not
 * numeric immediates. Routing the constant through a function arg
 * forces pcc to use `li` (which lowers to lui+ori at the asm level)
 * instead. */
static void
copy_listing_to_va(unsigned int va, int n)
{
	char *dst = (char *)va;
	int i;
	for (i = 0; i < n; i++) dst[i] = list_buf[i];
}

/* Service an op=5 SUP_OP_LIST_TASKS. Walks the libc task table,
 * formats one line per live slot into a temp buffer, ObjAllocs a
 * matching-size TAG_DATA bytes object, copies the text in via a
 * temporary R+W mapping, Unmaps, SENDs the ref to the requester,
 * then ObjFreeDeferred's the bytes (drain window so the shell can
 * still OBJ_READ_REQ from it).
 *
 * The dequeue has already filled O3 with the requester's reply_cap
 * (mirror of handle_spawn_request's contract). */
static void
handle_list_tasks_request(void)
{
	list_stash_reply_cap();
	int n = build_task_listing();

	/* Allocate a TAG_DATA bytes object sized to the actual text.
	 * ObjAlloc rejects size 0, so use 1 in the empty-list case
	 * (we reply length=0 regardless and the byte goes unread). */
	int alloc_size = (n > 0) ? n : 1;
	int reply_status = list_alloc_reply(alloc_size);
	if (reply_status != 0) {
		asm volatile("onull o14");
	} else if (n > 0) {
		int s = list_map_reply(alloc_size);
		if (s == 0) {
			copy_listing_to_va(LIST_REPLY_VA, n);
			s = list_unmap_reply(alloc_size);
		}
		if (s != 0) reply_status = s;
	}

	list_send_reply(reply_status, n);
	if (reply_status == 0) list_free_reply();
}

/* Service one spawn request that just landed on our queue. The
 * dequeue has filled:
 *   R3 = op          (1 = spawn)
 *   R4 = byte len
 *   R5 = target_pid  (TARGET_PID_LOCAL or a literal PROCID; Phase 45e)
 *   R6 = term_hint+1 (Phase 49: 0 = "no override," N+1 = "child runs
 *                     with terminal index N's services")
 *   O2 = bytes ref
 *   O3 = reply cap
 *
 * Placement logic (Phases 45e/49/51):
 *   target_pid == self || LOCAL     -> local spawn (LOCAL is the
 *                                      relay-pin marker; see
 *                                      TARGET_PID_LOCAL doc).
 *   target_pid == ANY (Phase 51)     -> round-robin: pick the next
 *                                      live CPU. If picked != self,
 *                                      relay with term_hint forwarded;
 *                                      else stay local. Receiver
 *                                      dir-walks /sys/term/<N>/*
 *                                      (Phase 49 pass-through) so the
 *                                      child runs with the requester's
 *                                      terminal regardless of host
 *                                      CPU.
 *   target_pid == literal != self    -> explicit `run @N` to a peer.
 *                                      Relay; forward term_hint.
 *
 * IMPORTANT: orx_spawn clobbers O1..O3 via its manifest restore
 * path; pick_next_cpu's dir_walk and relay_spawn_request both
 * clobber O1..O5 via their inner ReceiveQueuePoll. Stash O2 (bytes
 * ref) and O3 (reply_cap) IMMEDIATELY so we have one consistent
 * recovery point regardless of which branch fires.
 */
static void
handle_spawn_request(int len, int target_pid, int term_hint, int self_procid)
{
	/* Stash the dequeued O2 (bytes ref) and O3 (reply cap) FIRST,
	 * before any code path that touches OPRs — including
	 * reap_exited_tasks, whose orx_unload internally task_waits
	 * via O1 and would silently drop the request payload. */
	asm volatile(
		"orefst o2, %0(o12)\n"
		"orefst o3, %1(o12)"
		:
		: "i"(RELAY_BYTES_SLOT_OFFSET),
		  "i"(SUP_SCRATCH_SLOT_OFFSET)
	);

	/* Phase 54: reap before alloc. Each successful op=1 grabs a
	 * libc task-table slot; if previous spawns have exited but
	 * weren't reaped, the table eventually fills and orx_spawn
	 * returns -6. Sweep EXITED slots first so we hand the next
	 * spawn a clean table. */
	reap_exited_tasks();

	if (target_pid != TARGET_PID_LOCAL
	    && target_pid != TARGET_PID_ANY
	    && target_pid != self_procid) {
		/* Explicit `run @N` to a different CPU. */
		asm volatile(
			"orefld o2, %0(o12)\n"
			"orefld o3, %1(o12)"
			:
			: "i"(RELAY_BYTES_SLOT_OFFSET),
			  "i"(SUP_SCRATCH_SLOT_OFFSET)
			: "r1"
		);
		if (relay_spawn_request(len, target_pid, term_hint) == 0)
			return;
		reply_to_requester(-1, -1);
		return;
	}

	/* Phase 51: round-robin only fires when the caller asked for it
	 * (target_pid == TARGET_PID_ANY). TARGET_PID_LOCAL means "stay
	 * here" (used by relay-pinned packets — without this pin the
	 * receiver would round-robin again and we'd ping-pong). A
	 * literal target_pid == self_procid also stays local (handled
	 * by the explicit-relay branch above missing on equality). */
	if (target_pid == TARGET_PID_ANY) {
		int picked = pick_next_cpu(self_procid);
		if (picked != self_procid) {
			asm volatile(
				"orefld o2, %0(o12)\n"
				"orefld o3, %1(o12)"
				:
				: "i"(RELAY_BYTES_SLOT_OFFSET),
				  "i"(SUP_SCRATCH_SLOT_OFFSET)
				: "r1"
			);
			/* relay_spawn_request always sets target_pid =
			 * TARGET_PID_LOCAL on the wire, so the receiver
			 * pins to its own CPU. */
			if (relay_spawn_request(len, picked, term_hint) == 0)
				return;
			reply_to_requester(-1, -1);
			return;
		}
		/* picked == self → fall through to local spawn. */
	}

	/* Local spawn path. Restore O2 so read_spawn_request can
	 * ObjFetchBytes from the (still-live) bytes object. O3 stays
	 * parked in SUP_SCRATCH for reply_to_requester. */
	asm volatile(
		"orefld o2, %0(o12)"
		:
		: "i"(RELAY_BYTES_SLOT_OFFSET)
		: "r1"
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

	/* Phase 49: terminal-pass-through OPR injection. If the request
	 * carried a hint, dir-walk /sys/term/<N>/{console,keyboard,
	 * grid} and stash the resolved refs into ORX_SLOT_CHILD_O5/O6/
	 * O7. orx_task_create's swap dance picks them up around the
	 * upcoming TaskCreate and the child wakes up with the
	 * requester's terminal services in O5/O6/O7.
	 *
	 * Phase 51: ALSO propagate the terminal_idx itself (as an int)
	 * via orx_set_child_terminal_idx — orx_task_create stuffs it
	 * into R5 just before TaskCreate, the simulator copies to the
	 * child's R4, crt0 stashes to _orisc_init_r4, and the child's
	 * task_init reads it back into my_terminal_idx. That closes
	 * the loop: when the round-robin'd shell on a peer CPU later
	 * calls sup_spawn, ITS libc's R7 packs ITS terminal_idx,
	 * routing further spawns back to the user's terminal. */
	if (term_hint > 0) {
		int term_idx = term_hint - 1;
		populate_child_term_slots(term_idx);
		orx_set_child_terminal_idx(term_idx);
	}

	task_t t = sup_spawn_named(path, args, cwd);
	int status = (t < 0) ? (int)t : 0;

	/* Always clear the per-spawn overrides after orx_spawn so a
	 * subsequent local spawn (no hint) doesn't pick up stale
	 * state. */
	if (term_hint > 0) {
		clear_child_term_slots();
		orx_clear_child_terminal_idx();
	}

	reply_to_requester(t, status);

	/* Resume the new task — orx_spawn registers but doesn't
	 * resume; we want the task to actually run. */
	if (t >= 0) (void)task_resume(t);

	/* Yield to give the just-resumed child at least one quantum
	 * before we loop back to poll. Without this, any SEND already
	 * queued in our mailbox at this point (most commonly a
	 * worker's relayed op=2 shutdown — Phase 48
	 * relay_shutdown_to_leader) is picked up by the very next
	 * poll and triggers the cascade-kill before the spawned task
	 * ever ran. The user-visible symptom: the requester's shell
	 * session never starts because TaskCreate-then-TaskKill lands
	 * first. Yielding here lets the child run through crt0,
	 * task_init, term_init's keyboard subscribe, and the welcome
	 * banner SEND, until it blocks on term_getkey's
	 * RecvQueuePoll. See test_supervisor_session_manager.sh. */
	if (t >= 0) task_yield();
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
 * preference, or a literal PROCID for explicit `run @N` placement.
 *
 * Phase 49: also returns terminal_hint (R6 in the dequeued payload
 * = sender's R7). Encoding: 0 = "no override; child inherits this
 * supervisor's boot OPRs," N+1 = "child runs with terminal index N's
 * console/keyboard/grid (we dir_walk /sys/term/<N>/* before spawn)."
 * Set when a peer relays a spawn from a foreign-terminal'd shell,
 * unset when a child program calls sup_spawn directly. */
/* Phase 52: the leader's dispatch loop uses a finite timeout so it
 * wakes periodically to scan /sys/term for hot-attached terminals.
 * Workers stay on infinite-timeout polling — they don't run the
 * hot-attach scan, and a timeout wakeup would just cost a wire
 * round-trip with nothing to do.
 *
 * The timeout is in scheduler ticks. simorisc decrements it only
 * when the supervisor is the current task on its CPU, so the
 * effective wall-clock interval is "this many ticks of supervisor
 * being current" — i.e., mostly idle ticks. With idle ticks pacing
 * at ~1ms, HOT_ATTACH_POLL_TICKS = 5000 gives roughly 5-second
 * hot-attach latency under low load, while a busy shell session
 * (where the supervisor is frequently blocked) extends that
 * naturally — exactly the throttling we want.
 *
 * The timeout is delivered to poll_one_request via a static
 * because pcc-orisc trips on a 5th C arg ("adrput: illegal op 57"
 * — same constraint that bit Phase 51's sup_spawn_for_terminal and
 * Phase 52's ps handler). Caller sets poll_timeout_ticks before
 * calling poll_one_request; the default -1 (infinite) is
 * preserved for workers and the legacy contract. */
#define HOT_ATTACH_POLL_TICKS 5000
static int poll_timeout_ticks = -1;

static int
poll_one_request(int *out_op, int *out_len, int *out_target_pid,
                 int *out_term_hint)
{
	int status, op, len, target_pid, term_hint;
	int t = poll_timeout_ticks;
	asm volatile(
		"omov  o1, o9\n"               /* mailbox */
		"addu  r4, %5, r0\n"           /* timeout */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0\n"
		"addu  %4, r6, r0"
		: "=r"(status), "=r"(op), "=r"(len),
		  "=r"(target_pid), "=r"(term_hint)
		: "r"(t)
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	*out_op = op;
	*out_len = len;
	*out_target_pid = target_pid;
	*out_term_hint = term_hint;
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
	 * descriptor index across boots. */
	if (allocate_service_mailbox() != 0) {
		print_str("supervisor: failed to allocate spawn mailbox\n");
		return 1;
	}

	task_init();

	/* Phase 45f: boot O8 carries the directory mailbox sub-cap
	 * (wired by scripts/boot.sh's `--service "DIR_PID=1@9"` on
	 * the O8 slot). task_init has already copied O8 to
	 * BOOT_PARENT_SLOT; we additionally publish it into DIR_SLOT
	 * so dir.c finds it without going through the lazy
	 * "ask my parent supervisor" bootstrap. The supervisor IS
	 * a top-level program (oriscrun is its parent, not another
	 * supervisor), so dir.c's lazy path doesn't apply.
	 *
	 * This must run BEFORE hf_init, which clobbers O8 with the
	 * hostfsd reply mailbox — but BOOT_PARENT_SLOT is already
	 * set by task_init so we read from there, not from O8
	 * directly, and the order of OREFLD/OREFST relative to
	 * hf_init doesn't matter. */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"orefst o1, %1(o12)"
		:
		: "i"(BOOT_PARENT_SLOT_OFFSET), "i"(DIR_SLOT_OFFSET)
		: "r1"
	);

	/* Read PROCID early — Phase 47's directory walks need it to
	 * select /sys/term/<procid>/{console,keyboard,grid}, and the
	 * later self-register / banner / leader-only blocks read it
	 * too. */
	int procid    = read_procid();
	int is_leader = (procid == 0);

	/* Phase 51: declare our terminal_idx (procid in the current
	 * model) so children spawned via orx_spawn inherit it through
	 * the R5 → child.R4 → _orisc_init_r4 chain, and so any future
	 * sup_spawn from us routes through the right terminal. The
	 * crt0 stash captured `_orisc_init_r4 = 0` for us (oriscrun
	 * doesn't fill R5 for top-level boots), so task_init left
	 * my_terminal_idx = -1 — override here.
	 *
	 * Also bias the round-robin counter so this supervisor's first
	 * relay lands on the next CPU after self. With both leader and
	 * worker biased this way, the initial spread is symmetric:
	 * leader picks worker first, worker picks leader first,
	 * alternating cleanly thereafter. */
	task_set_my_terminal_idx(procid);
	next_cpu_counter = procid + 1;

	/* Phase 54: initialise the kill-on-detach mapping. -1 means "no
	 * login bound to that terminal slot." Populated below when a
	 * boot login is spawned, and by hot_attach_maybe_spawn for
	 * hot-attached terminals. */
	{
		int i;
		for (i = 0; i < HOT_ATTACH_MAX_TERMS; i++)
			terminal_login_task[i] = -1;
	}

	/* Phase 47: walk the directory for our boot service refs. After
	 * 45h+47, devices self-register at /sys/term/<N>/* and
	 * /sys/hostfsd/0 (oriscterm and hostfsd both do an inline-register
	 * SEND at startup). The supervisor's job here is to pick up those
	 * refs from the directory and stash them in O5/O6/O7/O10 — the
	 * boot OPR slots that hf_init reads, and that orx_task_create
	 * inherits to spawned children.
	 *
	 * In a fully directory-driven boot, the only --service wire each
	 * CPU needs is O8 = oriscdir. The walks below populate everything
	 * else from there. In a legacy boot with --service-wired O5/O6/O7/
	 * O10, the walks return -6 (no directory) and we keep the wired
	 * boot OPRs unchanged — same effective state, no breakage.
	 *
	 * Each walk publishes the resolved ref into DIR_RESULT_SLOT (616).
	 * We OREFLD it into the target OPR via inline asm. pcc treats
	 * O5/O6/O7/O10 as caller-save scratch but doesn't actually use
	 * them in the supervisor's main(), so the values survive until
	 * orx_spawn / hf_init consume them.
	 *
	 * Procid-aware: each CPU walks ITS OWN /sys/term/<procid>/*
	 * subtree, so a multi-terminal boot (Phase 46) with a real
	 * terminal per CPU lands the right binding without configuration. */
	{
		char path[PEER_PATH_BUF_SIZE];

		render_term_path(procid, "console", path);
		if (sup_walk_for_opr(path) == 0)
			asm volatile("orefld o5, %0(o12)"
			             :: "i"(DIR_RESULT_SLOT_OFFSET));

		render_term_path(procid, "keyboard", path);
		if (sup_walk_for_opr(path) == 0)
			asm volatile("orefld o6, %0(o12)"
			             :: "i"(DIR_RESULT_SLOT_OFFSET));

		render_term_path(procid, "grid", path);
		if (sup_walk_for_opr(path) == 0)
			asm volatile("orefld o7, %0(o12)"
			             :: "i"(DIR_RESULT_SLOT_OFFSET));

		if (sup_walk_for_opr("/sys/hostfsd/0") == 0)
			asm volatile("orefld o10, %0(o12)"
			             :: "i"(DIR_RESULT_SLOT_OFFSET));
	}

	/* Phase 56: WM-mediated leader session.  The leader CPU
	 * additionally tries to discover an oriscwm at /sys/wm/0; on
	 * success, replaces its boot O5/O6 with WM-derived
	 * console/keyboard caps so spawned children (sysinit, login,
	 * shell) see WM-mediated surfaces instead of direct terminal
	 * caps via Phase-49 inheritance.
	 *
	 * Workers stay on direct per-CPU /sys/term/<procid>/* — the
	 * milestone-3 WM only mediates the leader's session.  Multi-
	 * window tiling (a later milestone) will generalize this.
	 *
	 * Graceful degradation: if /sys/wm/0 doesn't resolve (no
	 * oriscwm running, or it crashed) wm_init returns negative and
	 * we keep the direct caps walked above.  The shell + login still
	 * work, just without WM mediation.
	 *
	 * We pass O1 = null as the owner-ref to wm_new_window — the
	 * supervisor never EXITs (it's a long-running daemon), so
	 * task_query auto-destroy isn't useful here.  The window stays
	 * alive for the supervisor's lifetime.  */
	if (is_leader) {
		/* Boot race: the WM lives on its own CPU and may still be
		 * mid-init when we get here.  Retry wm_init briefly on
		 * WIN_E_NOENT (-2 = "/sys/wm/0 didn't resolve yet"); same
		 * cadence as sup_walk_for_opr's per-CPU /sys/term retry.
		 * On systems with no WM at all, the directory walk
		 * returns NOT_FOUND every time and we exit the loop after
		 * 5 attempts to fall back to direct terminal — adds a few
		 * task_yields of latency for the no-WM case, which is
		 * cheap. */
		int wm_status, attempt;
		for (attempt = 0; attempt < 5; attempt++) {
			wm_status = wm_init();
			if (wm_status == 0)              break;
			if (wm_status != WIN_E_NOENT)    break;
			task_yield();
		}
		if (wm_status == 0) {
			int wid, w_cells, h_cells;
			asm volatile("onull o1");
			int rc = wm_new_window(WIN_TYPE_CONSOLE, &wid,
			                       &w_cells, &h_cells);
			if (rc == 0) {
				/* Bind console + keyboard.  The resolved cap
				 * lands in DIR_RESULT_SLOT (offset 616 — wm.c
				 * shares the slot with dir_walk's result).
				 * OREFLD into the supervisor's working OPR.
				 *
				 * For CONSOLE we ALSO stash the resolved cap
				 * into WM_LEADER_CONSOLE_SLOT so populate_child_-
				 * term_slots can hand it down to spawned children
				 * (login, shell) targeting the leader's own
				 * terminal.  Without that, children walk
				 * /sys/term/0/console directly and their output
				 * bypasses the WM. */
				if (wm_bind_surface(wid, WSURF_CONSOLE) == 0) {
					asm volatile(
						"orefld o5, %0(o12)\n"
						"orefst o5, %1(o12)"
						:
						: "i"(DIR_RESULT_SLOT_OFFSET),
						  "i"(WM_LEADER_CONSOLE_SLOT_OFFSET)
					);
				}
				if (wm_bind_surface(wid, WSURF_KEYBOARD) == 0) {
					asm volatile("orefld o6, %0(o12)"
					             :: "i"(DIR_RESULT_SLOT_OFFSET));
				}
				SUP_PRINT("supervisor: WM-mediated leader "
				          "session (wid=");
				SUP_PRINT_INT(wid);
				SUP_PRINT(")\n");
			} else {
				SUP_PRINT("supervisor: wm_new_window failed (");
				SUP_PRINT_INT(rc);
				SUP_PRINT(") — using direct terminal\n");
			}
		} else if (wm_status != WIN_E_NOENT && wm_status != WIN_E_IO) {
			/* WIN_E_NOENT (-2) and WIN_E_IO (-6) are the expected
			 * "no WM available" returns; quiet.  Anything else
			 * is unexpected. */
			SUP_PRINT("supervisor: wm_init returned ");
			SUP_PRINT_INT(wm_status);
			SUP_PRINT(" — using direct terminal\n");
		}
	}

	hf_init();
	orx_init();

	/* Make every subsequent TaskCreate inherit a fresh sub-cap
	 * of our mailbox in O8. orx_task_create reads
	 * ORX_SLOT_CHILD_O8 and does the swap transparently. */
	install_child_o8_override();

	print_str(is_leader ? banner_leader : banner_worker);

	/* Phase 45f: register self at /sys/cpu/<procid>/supervisor.
	 * Peers find us via dir_walk on this path; relay_spawn_request
	 * uses it to discover where to forward op=1 requests. The ref
	 * we publish is a fresh R+S sub-cap of our spawn mailbox (O9
	 * holds the full ref). */
	{
		char peer_path[PEER_PATH_BUF_SIZE];
		render_peer_path(procid, peer_path);

		/* Derive R+S sub-cap of our mailbox into O1, then call
		 * dir_register which uses O1 as the ref to publish. */
		int derive_status;
		asm volatile(
			"omov   o1, o9\n"
			"addiu  r4, r0, 9\n"          /* R | S */
			"call   #0x103\n"             /* ObjDerive */
			"nop\n"
			"addu   %0, r2, r0"
			: "=r"(derive_status) : : "r1", "r2", "r4"
		);
		if (derive_status != 0) {
			SUP_PRINT("supervisor: ObjDerive for self-register failed\n");
			return 1;
		}
		int reg_status = dir_register(peer_path);
		if (reg_status != 0) {
			SUP_PRINT("supervisor: dir_register failed: ");
			SUP_PRINT_INT(reg_status);
			SUP_PRINT("\n");
			/* Non-fatal in degenerate single-CPU configurations
			 * where no directory is wired; fall through.
			 * In multi-CPU we expect this to succeed. */
		}
	}

	/* Phase 54: leader subscribes to /sys/term so hot-attach can
	 * react to terminal add/remove events without polling. The
	 * notify_cap is a fresh R+S sub-cap of our spawn mailbox (O9);
	 * notifications arrive there with R3 = SUP_OP_DIR_NOTIFY,
	 * which the dispatch loop routes to a fresh hot-attach scan.
	 *
	 * If the subscribe fails (e.g., no oriscdir wired in a
	 * degenerate test config), the periodic-poll fallback in the
	 * dispatch loop still picks up changes — just at the longer
	 * HOT_ATTACH_POLL_TICKS cadence rather than the wire-round-trip
	 * latency of subscriptions. */
	if (is_leader) {
		int derive_status;
		asm volatile(
			"omov   o1, o9\n"
			"addiu  r4, r0, 9\n"          /* CAP_R | CAP_S */
			"call   #0x103\n"             /* ObjDerive */
			"nop\n"
			"addu   %0, r2, r0"
			: "=r"(derive_status) : : "r1", "r2", "r4"
		);
		if (derive_status == 0) {
			int sub_status = dir_subscribe("/sys/term",
			                               SUP_OP_DIR_NOTIFY);
			if (sub_status != 0) {
				SUP_PRINT("supervisor: /sys/term subscribe failed (");
				SUP_PRINT_INT(sub_status);
				SUP_PRINT(") — periodic poll fallback only\n");
			} else {
				SUP_PRINT("supervisor: /sys/term subscribed\n");
			}
		}
	}

	/* Phase 55: the /programs MOUNT is no longer the supervisor's
	 * problem. oriscdir reads its --config file at startup and
	 * stages the mount as a deferred intent; when hostfsd's
	 * self-register lands at /sys/hostfsd/0, oriscdir applies the
	 * mount automatically. By the time the leader gets here, walks
	 * for /programs/* resolve through the directory without any
	 * supervisor-side dir_mount call. (See
	 * tools/devices/oriscdir.default.conf for the canonical config.)
	 *
	 * In degraded test configs without a directory daemon, the
	 * mount obviously isn't present — orx_spawn's vfs_open falls
	 * back to direct hf_open, same as the pre-Phase-55 dir_mount-
	 * failed path did.
	 *
	 * Sysinit still spawns here as a one-shot "system setup" hook;
	 * it currently doesn't do much, but the slot is available for
	 * late-boot setup work that's CPU-local (i.e., something a
	 * directory mutation can't express). */
	if (is_leader) {
		task_t sysinit = sup_spawn_named(SYSINIT_PATH, "", "/");
		if (sysinit < 0) {
			SUP_PRINT("supervisor: failed to spawn sysinit: ");
			SUP_PRINT_INT((int)sysinit);
			SUP_PRINT(" — continuing\n");
		} else if (task_resume(sysinit) != 0) {
			SUP_PRINT("supervisor: failed to resume sysinit\n");
		}
	}

	/* Phase 47: device registration is no longer the supervisor's job —
	 * oriscterm and hostfsd self-register at /sys/term/<N>/* and
	 * /sys/hostfsd/<N> respectively via the inline-register wire op
	 * (DIR_OP_REG_INLINE = 5; see tools/devices/oriscdir's docstring).
	 * The supervisor walked the directory above to populate its own
	 * O5/O6/O7/O10 working OPRs; those are the boot ABI for spawned
	 * children, not registry inputs.
	 *
	 * has_terminal is now a property of the directory walk: O5
	 * non-null after the walks-above means we have a console to
	 * spawn a shell against. The OISN probe matches the wired-boot
	 * fallback case too (boot.sh that still wires --service for O5
	 * lands the same value via boot wiring; the walk fails -6, the
	 * orefld doesn't run, and O5 keeps the wired value). */
	int has_terminal;
	asm volatile("oisn %0, o5" : "=r"(has_terminal));
	has_terminal = !has_terminal;     /* OISN sets 1 when null */

	/* Phase 46: any CPU with a terminal spawns its own shell, bound
	 * to its own boot O5/O6/O7. Multiple oriscterm instances + per-
	 * CPU terminal wiring (boot.sh launches term pid=16 for CPU 0,
	 * term pid=19 for CPU 1, etc.) gives users one shell per Tk
	 * window, all sharing the same /programs mount and supervisor-
	 * mediated spawn machinery.
	 *
	 * Pre-Phase-46 boots had the shell-spawn gated on `is_leader`
	 * (procid==0). Now it's gated on `has_terminal`: CPUs without
	 * a terminal stay in the dispatch loop as before (servicing
	 * relayed spawn requests but doing no UI), CPUs with a terminal
	 * become shell hosts. In a single-terminal config (existing test
	 * harnesses that wire term to procid 0 only, null pads for procid
	 * 1) that collapses to the prior behaviour. */
	if (has_terminal) {
		/* Phase 47: workers race the leader's /programs mount (now
		 * done by sysinit.orx — see the leader-only block above).
		 * If we try to spawn login before /programs is mounted in
		 * oriscdir, orx_spawn → vfs_open returns NOT_FOUND → -1.
		 * Wait for the mount by walking it; only proceed once
		 * sysinit has installed it. On the leader the previous
		 * task_wait on sysinit guarantees this without waiting; on
		 * workers we may need a few retries.
		 *
		 * 50 attempts × ~ms-scale wire round-trips ≈ tens of ms; in
		 * practice sysinit lands well before any worker reaches
		 * this point under any reasonable boot ordering. */
		{
			int kind, attempt;
			char rem[16];
			for (attempt = 0; attempt < 50; attempt++) {
				int rc = dir_walk("/programs", &kind, rem, sizeof(rem));
				if (rc >= 0 && kind == DIR_KIND_MOUNT) break;
				if (rc == -6) break;     /* no directory; legacy boot */
				task_yield();
			}
		}
		task_t login = sup_spawn_named(LOGIN_PATH, "", "/");
		if (login < 0) {
			SUP_PRINT("supervisor: failed to spawn login: ");
			SUP_PRINT_INT((int)login);
			SUP_PRINT("\n");
			return 1;
		}
		if (task_resume(login) != 0) {
			SUP_PRINT("supervisor: failed to resume login\n");
			return 1;
		}
		(void)login;   /* login-/shell-exit detection is via op=2 SEND
		                * below, not task_query. login.orx loops the
		                * welcome-banner / shell-spawn cycle; only
		                * the shell's `exit`/`quit` (which calls
		                * sup_shutdown) actually halts us. */

		/* Phase 54: record this CPU's boot login so the
		 * kill-on-detach scan can reap it if its terminal goes
		 * away. The mapping is by terminal index — and our boot
		 * terminal IS our procid (per the per-supervisor
		 * /sys/term/<procid>/* dir-walks above), so the array
		 * slot is procid. */
		if (procid >= 0 && procid < HOT_ATTACH_MAX_TERMS) {
			terminal_login_task[procid] = login;
		}
	}

	/* Phase 52: seed hot_attach_seen with the terminals already
	 * registered at boot time. Per-supervisor has_terminal blocks
	 * (above) handled them; the dispatch-loop hot-attach scan
	 * should only act on terminals that appear AFTER this point.
	 * Leader-only — workers don't run the scan, so they don't need
	 * the seed either. */
	if (is_leader) {
		hot_attach_walk(hot_attach_mark);
		SUP_PRINT("supervisor: hot-attach seeded\n");
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
	/* Leader: enable hot-attach polling. Workers leave the timeout
	 * at -1 (infinite) — they don't run the scan. */
	if (is_leader) {
		poll_timeout_ticks = HOT_ATTACH_POLL_TICKS;
	}

	for (;;) {
		int op, len, target_pid, term_hint;
		int status = poll_one_request(&op, &len, &target_pid,
		                              &term_hint);
		if (status != 0) {
			/* Either ETIMEOUT (leader's hot-attach pulse fired)
			 * or some other transient error. On the leader, reap
			 * any exited tasks (Phase 54) and run the hot-attach
			 * scan (Phase 52: spawn for new, Phase 54: kill for
			 * gone); on workers, just try again. */
			if (is_leader) {
				reap_exited_tasks();
				hot_attach_scan();
			}
			continue;
		}

		if (op == 1) {
			handle_spawn_request(len, target_pid, term_hint, procid);
		} else if (op == 2 && has_terminal) {
			/* Cascade-kill every task we own before halting.
			 * Phase 48: login.orx is parked in task_wait on
			 * shell, and a clean shell exit (logout) wakes it
			 * naturally; but `exit`/`quit` SENDs us op=2 from
			 * the shell and yield-loops, so login is still
			 * BLOCKED here. Without this kill cascade, login
			 * would resume after our halt-tear-down — too
			 * late, but visibly racing the screen wipe in
			 * real Tk timing. Killing it deterministically
			 * before we return guarantees the supervisor's
			 * task table is clean when oriscrun's leader
			 * watchdog fires SIGTERM. */
			unsigned int mask = task_active_mask();
			int t;
			for (t = 0; t < TASK_MAX_CONCURRENT; t++) {
				if (mask & (1u << t)) {
					(void)task_kill((task_t)t, 137);
				}
			}
			/* Phase 48: workers relay op=2 to the leader so
			 * the leader's exit (CPU 0) trips oriscrun's
			 * --leader watchdog, which SIGTERMs the rest of
			 * the process group. Without this, `exit` from a
			 * worker terminal halts only that CPU, and the
			 * simulator stays alive until --leader-timeout
			 * (10 minutes by default in boot.sh) runs out.
			 * Leader skips the relay — its own halt is the
			 * trigger oriscrun is watching for. */
			if (procid != 0) {
				(void)relay_shutdown_to_leader();
			}
			SUP_PRINT(shell_done);
			return 0;
		} else if (op == 4) {
			/* SUP_OP_GET_DIR (Phase 45g): a child program's dir.c
			 * is asking us for the directory mailbox so it can
			 * populate its own DIR_SLOT. Reply with our DIR_SLOT
			 * in O2 (= oriscdir's primary mailbox sub-cap, copied
			 * from BOOT_PARENT_SLOT at boot).
			 *
			 * Phase 48: probe DIR_SLOT first. If it's null (no
			 * oriscdir wired into this supervisor — degenerate
			 * single-CPU test config), reply with status -6 (EIO)
			 * so the child's dir_init bails cleanly instead of
			 * happily caching a null DIR_SLOT and then trapping
			 * on its first dir_walk's SEND-to-null. */
			int dir_isn;
			asm volatile(
				"orefld o1, %1(o12)\n"
				"oisn   %0, o1"
				: "=r"(dir_isn)
				: "i"(DIR_SLOT_OFFSET)
				: "r1"
			);
			int reply_status = dir_isn ? -6 : 0;
			asm volatile(
				"omov   o1, o3\n"            /* recipient = caller's reply_cap */
				"orefld o2, %1(o12)\n"       /* O2 = our DIR_SLOT (null on -6) */
				"onull  o3\n"
				"addu   r4, %0, r0\n"        /* status: 0 OK or -6 EIO */
				"addiu  r5, r0, 0\n"
				"addiu  r6, r0, 0\n"
				"addiu  r7, r0, 0\n"
				"send   o1\n"
				:
				: "r"(reply_status), "i"(DIR_SLOT_OFFSET)
				: "r1", "r4", "r5", "r6", "r7"
			);
		} else if (op == SUP_OP_LIST_TASKS) {
			/* Phase 52: cross-CPU `ps`. Walk our task table,
			 * format one line per live slot, send a TAG_DATA
			 * bytes ref back to the requester. */
			handle_list_tasks_request();
		} else if (op == SUP_OP_DIR_NOTIFY) {
			/* Phase 54: oriscdir tells us /sys/term mutated.
			 * Re-run the scan so any newly-attached terminal
			 * gets a login (and any departed terminal's login
			 * gets killed, once oriscdir grows entry-removal). */
			if (is_leader) {
				reap_exited_tasks();
				hot_attach_scan();
			}
		} else {
			SUP_PRINT(unknown_op);
		}
	}
}

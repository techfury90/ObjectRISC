/*
 * dir.c — Phase 45f: client-side libc for the oriscdir directory
 * service.
 *
 * Three pieces:
 *
 *   - dir_init (lazy, internal): ensures DIR_SLOT in O12 holds a
 *     usable mailbox sub-cap to oriscdir. Supervisors get this
 *     for free at boot (their boot O8 IS the directory, harvested
 *     to BOOT_PARENT_SLOT, copied into DIR_SLOT explicitly by
 *     supervisor.c::main). Other programs (shells, etc.) lazily
 *     query their parent supervisor for the directory ref via the
 *     supervisor's op=4 (get_dir) reply protocol on first call.
 *
 *   - The four wire ops: dir_register, dir_mount, dir_walk,
 *     dir_list. Each one packs a path bytes object via the same
 *     ObjAlloc/MapObject/Unmap dance sup.c uses, SENDs to the
 *     directory, blocks on the per-program reply mailbox (shared
 *     with sup.c's REPLY_MB_SLOT — same slot, same lazy alloc),
 *     decodes the response.
 *
 *   - Helpers: a small ObjFetchBytes wrapper for pulling list /
 *     remainder bytes out of caller-allocated dest buffers, and
 *     ObjFreeDeferred cleanups for the per-call bytes objects.
 *
 * The wire protocol is documented in tools/devices/oriscdir's
 * module docstring. Mirror as needed when changing fields.
 */

#include "liborisc.h"

#define TAG_DATA           0x4102
#define TAG_SERVICE        0x4103

#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* Wire ops on oriscdir's mailbox (must match the daemon). */
#define DIR_OP_REGISTER  1
#define DIR_OP_MOUNT     2
#define DIR_OP_WALK      3
#define DIR_OP_LIST      4
#define DIR_OP_SUBSCRIBE 6   /* Phase 54 — register a notify_cap for
                              * mutations under a path. See oriscdir
                              * docstring for the wire details. */

/* Supervisor's get-dir-ref op (handled by supervisor.c — Phase
 * 45f). Shells and other supervisor-spawned programs SEND op=4
 * to their BOOT_PARENT_SLOT (= local supervisor) to bootstrap
 * their DIR_SLOT. The supervisor replies with O2 = its own
 * directory mailbox sub-cap. */
#define SUP_OP_GET_DIR  4

/* Stack VA layout (CONTRACT.md §2). For ObjFetchBytes destinations
 * we use the boot stack ref O11 plus a computed offset; same trick
 * the supervisor's read_spawn_request uses. */
#define STACK_BOTTOM     0x001f0000

/* Per-call buffers. Paths can be a few hundred bytes
 * comfortably; remainders (mount prefix + leftover) are similar. */
#define DIR_PATH_BUF_SIZE    256
#define DIR_REM_BUF_SIZE     512

/* Byte offsets within O12 of the libc-managed slots dir.c reaches
 * for. Mirrored as private constants here; task.c keeps the
 * canonical definitions. */
#define BOOT_PARENT_SLOT_OFFSET   544
#define REPLY_MB_SLOT_OFFSET      552
#define DIR_SLOT_OFFSET           584

/* Park-spaces around primitives that clobber O1..O3 (ObjAlloc,
 * MapObject, etc.). Layout-equivalent to sup.c's SUP_SCRATCH; we
 * piggyback on the same slot here since the two clients never
 * have a reply outstanding simultaneously (both are synchronous
 * SEND-and-poll). Keep the offset in sync with task.c. */
#define DIR_SCRATCH_SLOT_OFFSET   576

/* Stash for derived reply sub-caps. We OREFST into here right
 * after ObjDerive, then OREFLD directly into O3 at SEND time —
 * never touching O15 (which task.c uses for the boot data ref).
 * Mirrors task.c's DIR_REPLY_SCRATCH. */
#define DIR_REPLY_SCRATCH_OFFSET  608

/* dir_walk publishes its resolved ref here on return. Callers
 * OREFLD from this slot rather than relying on O1 being preserved
 * across the function-call boundary — pcc treats OPRs as scratch
 * so cross-function ref returns via OPR are unreliable. */
#define DIR_RESULT_SLOT_OFFSET    616

/* dir_register / dir_mount stash the caller-supplied O1 (the ref to
 * publish) here AT FUNCTION ENTRY, before dir_init or
 * dir_reply_mailbox_init run — both of those clobber O1 internally
 * (oisn probes use orefld; ObjAlloc on first call leaves the new
 * REPLY_MB ref in O1). Without this slot the wire SEND ends up
 * registering REPLY_MB instead of the caller's intended ref. The
 * SEND that emits the wire op OREFLDs from this slot into O4.
 * Phase 45f bugfix. Mirror task.c's DIR_INPUT_REF_SLOT. */
#define DIR_INPUT_REF_SLOT_OFFSET 624

/* OISN-style probe of a slot. Returns 1 (true) when the slot is
 * literal-zero/null, 0 otherwise. Same idiom sup.c uses. */
static int
dir_slot_isn(int offset)
{
	int isn;
	switch (offset) {
	case DIR_SLOT_OFFSET:
		asm volatile("orefld o1, 584(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		break;
	case BOOT_PARENT_SLOT_OFFSET:
		asm volatile("orefld o1, 544(o12)\noisn %0, o1"
		             : "=r"(isn) : : "r1");
		break;
	default:
		isn = 1;
	}
	return isn;
}

/* Allocate the per-program reply mailbox and park its full ref in
 * REPLY_MB_SLOT. Idempotent: subsequent calls fast-return. Mirrors
 * sup.c's sup_reply_mailbox_init; both clients call it. */
static int
dir_reply_mailbox_init(void)
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
		"addiu r6, r0, %2\n"
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

	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 4\n"
		"call  #0x203\n"              /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status) : : "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Pack `len` bytes from `src` (a stack/data string) into the
 * TAG_DATA bytes object whose ref is in O1. Reuses the
 * MapObject-into-temporary-VA dance sup.c::sup_pack_request uses;
 * factored locally rather than shared because the two functions
 * have slightly different VA conventions and we don't yet have a
 * shared-libc-helper layer. The mapping VA is 0x600000 (above
 * the long-lived argv mapping at 0x500000) so dir.c and sup.c
 * can coexist within one task without VA conflicts. */
#define DIR_PACK_VA  0x00600000

static int
dir_pack_bytes_o1(const char *src, int len, int buf_size)
{
	int status;
	/* Save the bytes ref in O14 (sup.c's convention) so MapObject
	 * doesn't lose it. */
	asm volatile("omov o14, o1");

	asm volatile(
		"omov  o1, o14\n"
		"lui   r4, 0x60\n"
		"addu  r5, r0, r0\n"
		"addiu r6, r0, %1\n"          /* R+W */
		"addu  r7, %2, r0\n"
		"call  #0x110\n"              /* MapObject */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_W), "r"(buf_size)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) return status;

	/* Copy bytes into the mapping. pcc rejects (char *)0x600000 as
	 * a literal cast (emits an `la r, N` pseudo asmorisc doesn't
	 * accept); synthesize the VA via lui+ori the way sup.c does. */
	char *dst;
	asm volatile(
		"lui  %0, 0x60\n"
		"ori  %0, %0, 0"
		: "=r"(dst)
	);
	int i;
	for (i = 0; i < len && i + 1 < buf_size; i++)
		dst[i] = src[i];

	asm volatile(
		"lui   r4, 0x60\n"
		"addu  r5, %1, r0\n"
		"call  #0x111\n"              /* Unmap */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(buf_size)
		: "r2", "r3", "r4", "r5"
	);
	return status;
}

/* Allocate a fresh TAG_DATA bytes object, parks the ref in O14
 * (the convention sup.c established). Returns 0 OK. */
static int
dir_alloc_bytes_o14(int size, int caps)
{
	int status;
	asm volatile(
		"addu  r4, %1, r0\n"
		"addiu r5, r0, %2\n"          /* TAG_DATA */
		"addu  r6, %3, r0\n"
		"call  #0x100\n"
		"nop\n"
		"omov  o14, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(size), "i"(TAG_DATA), "r"(caps)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* Free O14 with a generous drain so any remaining OBJ_READ_REQs
 * from the daemon land before reclamation. */
static void
dir_free_o14_deferred(void)
{
	asm volatile(
		"omov  o1, o14\n"
		"addiu r4, r0, 1500\n"
		"call  #0x107\n"              /* ObjFreeDeferred */
		"nop"
		: : : "r2", "r3", "r4"
	);
}

/* String length, local. */
static int
dir_strlen(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	return n;
}

/* Lazily ensure DIR_SLOT is populated. Strategy:
 *   1. If DIR_SLOT is already non-null: nothing to do.
 *   2. Otherwise SEND op=SUP_OP_GET_DIR to BOOT_PARENT_SLOT
 *      (= local supervisor's mailbox) and wait on REPLY_MB_SLOT
 *      for the reply. The supervisor's reply puts its DIR_SLOT
 *      ref into O2; we OREFST it into our own DIR_SLOT.
 *
 * Supervisors themselves are expected to have already populated
 * DIR_SLOT directly from BOOT_PARENT_SLOT at boot — they don't go
 * through this query path. */
static int
dir_init(void)
{
	if (!dir_slot_isn(DIR_SLOT_OFFSET))
		return 0;        /* already populated */

	if (dir_slot_isn(BOOT_PARENT_SLOT_OFFSET))
		return -6;       /* no parent — can't bootstrap */

	int status = dir_reply_mailbox_init();
	if (status != 0) return status;

	/* Derive R+S sub-cap from our reply mailbox. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"           /* R|S */
		"call  #0x103\n"              /* ObjDerive → O1 */
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status) : : "r1", "r2", "r4"
	);
	if (status != 0) return status;

	/* SEND op=SUP_OP_GET_DIR to the supervisor.
	 *   O1 = supervisor sub-cap (recipient)
	 *   O3 = reply sub-cap
	 *   R4 = op = 4 */
	asm volatile(
		"orefld o1, 544(o12)\n"
		"onull  o2\n"
		"orefld o3, 608(o12)\n"
		"addiu  r4, r0, %0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(SUP_OP_GET_DIR)
		: "r1", "r4", "r5", "r6", "r7"
	);

	/* Block on reply mailbox. The supervisor's reply puts the
	 * directory ref in O2. */
	int reply_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"              /* ReceiveQueuePoll */
		"nop\n"
		"orefst o2, 584(o12)\n"       /* DIR_SLOT */
		"addu  %0, r3, r0\n"          /* receiver R3 = caller R4 = status */
		"addu  %1, r2, r0"
		: "=r"(reply_status), "=r"(status)
		: : "r1", "r2", "r3", "r4"
	);
	if (status != 0) return status;
	if (reply_status != 0) return reply_status;
	return 0;
}

/* dir_register — bind O1 (caller-supplied) to `path` as a leaf. */
int
dir_register(const char *path)
{
	/* Save the caller's O1 (the ref to register) into the dedicated
	 * input slot IMMEDIATELY — before any other call. dir_init's
	 * dir_slot_isn does `orefld o1, ...` which clobbers O1; on first
	 * dir_reply_mailbox_init the inner ObjAlloc leaves REPLY_MB's
	 * ref in O1. If we save to O13 (or elsewhere) AFTER those calls,
	 * we'd register the wrong ref. The SEND below pulls O4 from this
	 * slot directly. */
	asm volatile(
		"orefst o1, %0(o12)"
		:
		: "i"(DIR_INPUT_REF_SLOT_OFFSET)
	);

	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int len = dir_strlen(path);
	if (len <= 0 || len >= DIR_PATH_BUF_SIZE) return -1;

	rc = dir_alloc_bytes_o14(DIR_PATH_BUF_SIZE,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (rc != 0) return rc;
	rc = dir_pack_bytes_o1(path, len, DIR_PATH_BUF_SIZE);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	/* Derive R+S reply sub-cap. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"
		"call  #0x103\n"
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(rc) : : "r1", "r2", "r4"
	);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	/* SEND to oriscdir.
	 *   O1 = dir mailbox       (DIR_SLOT)
	 *   O2 = path bytes        (O14)
	 *   O3 = reply_cap         (DIR_REPLY_SCRATCH)
	 *   O4 = ref to register   (DIR_INPUT_REF_SLOT, saved at entry)
	 *   R4 = op
	 *   R5 = path length */
	asm volatile(
		"orefld o1, 584(o12)\n"        /* DIR_SLOT */
		"omov   o2, o14\n"
		"orefld o3, 608(o12)\n"        /* DIR_REPLY_SCRATCH */
		"orefld o4, %2(o12)\n"         /* DIR_INPUT_REF_SLOT */
		"addiu  r4, r0, %0\n"
		"addu   r5, %1, r0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(DIR_OP_REGISTER), "r"(len),
		    "i"(DIR_INPUT_REF_SLOT_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);

	dir_free_o14_deferred();

	int status, reply_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r2, r0"
		: "=r"(reply_status), "=r"(status)
		: : "r1", "r2", "r3", "r4"
	);
	if (status != 0) return -6;
	return reply_status;
}

/* dir_mount — register a MOUNT at `path` with O1 as the service
 * ref and `prefix` as the path inside the mounted service. The
 * daemon expects path and prefix concatenated with NULs in one
 * bytes object, with R5 = path length, R6 = prefix length. */
int
dir_mount(const char *path, const char *prefix)
{
	/* Save the caller's O1 (service ref) into the input slot
	 * IMMEDIATELY — same reason as dir_register. dir_init and
	 * dir_reply_mailbox_init both clobber O1 internally. */
	asm volatile(
		"orefst o1, %0(o12)"
		:
		: "i"(DIR_INPUT_REF_SLOT_OFFSET)
	);

	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int plen = dir_strlen(path);
	int xlen = dir_strlen(prefix);
	if (plen <= 0 || plen + 1 + xlen + 1 > DIR_PATH_BUF_SIZE)
		return -1;

	rc = dir_alloc_bytes_o14(DIR_PATH_BUF_SIZE,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (rc != 0) return rc;

	/* Pack path\0prefix\0. We do it in two ops: first the path,
	 * then ANOTHER mapping cycle for the prefix. (We could just
	 * pack everything in one map; this is slightly clearer.) */
	int status;
	asm volatile(
		"omov  o1, o14\n"
		"lui   r4, 0x60\n"
		"addu  r5, r0, r0\n"
		"addiu r6, r0, %1\n"
		"addiu r7, r0, %2\n"
		"call  #0x110\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_W), "i"(DIR_PATH_BUF_SIZE)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) { dir_free_o14_deferred(); return status; }

	char *buf;
	asm volatile(
		"lui  %0, 0x60\n"
		"ori  %0, %0, 0"
		: "=r"(buf)
	);
	int i, n = 0;
	for (i = 0; i < plen && n + 1 < DIR_PATH_BUF_SIZE; i++)
		buf[n++] = path[i];
	if (n + 1 < DIR_PATH_BUF_SIZE) buf[n++] = '\0';
	for (i = 0; i < xlen && n + 1 < DIR_PATH_BUF_SIZE; i++)
		buf[n++] = prefix[i];
	if (n + 1 < DIR_PATH_BUF_SIZE) buf[n++] = '\0';

	asm volatile(
		"lui   r4, 0x60\n"
		"addiu r5, r0, %1\n"
		"call  #0x111\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(DIR_PATH_BUF_SIZE)
		: "r2", "r3", "r4", "r5"
	);
	if (status != 0) { dir_free_o14_deferred(); return status; }

	/* Derive reply sub-cap. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"
		"call  #0x103\n"
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status) : : "r1", "r2", "r4"
	);
	if (status != 0) { dir_free_o14_deferred(); return status; }

	/* SEND.
	 *   O1 = dir mailbox       (DIR_SLOT)
	 *   O2 = path+prefix bytes (O14)
	 *   O3 = reply_cap         (DIR_REPLY_SCRATCH)
	 *   O4 = service ref       (DIR_INPUT_REF_SLOT, saved at entry)
	 *   R5 = path length, R6 = prefix length */
	asm volatile(
		"orefld o1, 584(o12)\n"
		"omov   o2, o14\n"
		"orefld o3, 608(o12)\n"
		"orefld o4, %3(o12)\n"
		"addiu  r4, r0, %0\n"
		"addu   r5, %1, r0\n"
		"addu   r6, %2, r0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(DIR_OP_MOUNT), "r"(plen), "r"(xlen),
		    "i"(DIR_INPUT_REF_SLOT_OFFSET)
		: "r1", "r4", "r5", "r6", "r7"
	);

	dir_free_o14_deferred();

	int reply_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r2, r0"
		: "=r"(reply_status), "=r"(status)
		: : "r1", "r2", "r3", "r4"
	);
	if (status != 0) return -6;
	return reply_status;
}

/* dir_walk — resolve `path`. On success returns the remainder
 * length (0 for DIR/LEAF, >=0 for MOUNT). O1 holds the resolved
 * ref (LEAF target / MOUNT service, null for DIR). *kind_out is
 * the node kind. For MOUNT remainder_buf gets the prefix-and-
 * leftover path bytes (NUL-terminated within remainder_cap). */
int
dir_walk(const char *path, int *kind_out,
         char *remainder_buf, int remainder_cap)
{
	if (kind_out) *kind_out = DIR_KIND_NOT_FOUND;

	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int len = dir_strlen(path);
	if (len <= 0 || len >= DIR_PATH_BUF_SIZE) return -1;

	/* Allocate path bytes. */
	rc = dir_alloc_bytes_o14(DIR_PATH_BUF_SIZE,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (rc != 0) return rc;
	rc = dir_pack_bytes_o1(path, len, DIR_PATH_BUF_SIZE);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	/* Compute remainder dest as the offset of remainder_buf within
	 * the boot stack object — daemon writes via OBJ_WRITE_REQ
	 * through O11 (boot stack ref). Same pattern as supervisor's
	 * read_spawn_request. */
	int dst_offset = (int)((unsigned int)remainder_buf - STACK_BOTTOM);

	/* Stash the boot stack ref into O13 so the SEND can pass it
	 * as the dest buffer in O4. */
	asm volatile("omov o13, o11");

	/* Derive reply sub-cap. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"
		"call  #0x103\n"
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(rc) : : "r1", "r2", "r4"
	);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	/* SEND op=walk.
	 *   O1 = dir mailbox
	 *   O2 = path bytes (O14)
	 *   O3 = reply_cap (O15)
	 *   O4 = boot stack ref (O13) — the daemon writes the
	 *        remainder into our stack at dst_offset
	 *   R5 = path length
	 *   R6 = (carries dst_offset to the daemon? No: R6 is
	 *        capacity. We ignore the daemon's dst_offset; the
	 *        wire protocol writes at offset 0 inside the dest
	 *        ref. To make it land at our remainder_buf's
	 *        VA we'd need to pass dst_offset too — extending
	 *        the protocol. For MVP we use a per-task scratch
	 *        bytes object instead of the stack.) */

	/* Reset: actually use a fresh TAG_DATA scratch for the
	 * remainder. ObjFetchBytes-copy it into the caller's buffer
	 * after the reply lands. Simpler than passing stack offsets
	 * across the wire. */

	/* Allocate a remainder scratch object, park its ref in O13. */
	int rem_alloc_status;
	asm volatile(
		"addiu r4, r0, %1\n"
		"addiu r5, r0, %2\n"          /* TAG_DATA */
		"addiu r6, r0, %3\n"
		"call  #0x100\n"              /* ObjAlloc → O1 */
		"nop\n"
		"omov  o13, o1\n"
		"addu  %0, r2, r0"
		: "=r"(rem_alloc_status)
		: "i"(DIR_REM_BUF_SIZE), "i"(TAG_DATA),
		  "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (rem_alloc_status != 0) {
		dir_free_o14_deferred();
		return rem_alloc_status;
	}

	asm volatile(
		"orefld o1, 584(o12)\n"
		"omov   o2, o14\n"
		"orefld o3, 608(o12)\n"
		"omov   o4, o13\n"
		"addiu  r4, r0, %0\n"
		"addu   r5, %1, r0\n"
		"addiu  r6, r0, %2\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(DIR_OP_WALK), "r"(len), "i"(DIR_REM_BUF_SIZE)
		: "r1", "r4", "r5", "r6", "r7"
	);

	dir_free_o14_deferred();   /* path bytes — done with it */

	/* Block on reply. Reply payload:
	 *   R3 = status, R4 = kind, R5 = remainder length
	 *   O2 = resolved ref (LEAF target / MOUNT service / null) */
	int status, kind, rem_len, poll_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"              /* ReceiveQueuePoll */
		"nop\n"
		"omov  o1, o2\n"              /* park resolved ref in O1 */
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r2, r0"
		: "=r"(status), "=r"(kind), "=r"(rem_len), "=r"(poll_status)
		: : "r1", "r2", "r3", "r4", "r5"
	);

	/* Save resolved ref so subsequent operations don't clobber. */
	asm volatile("omov o14, o1");

	if (poll_status != 0) {
		/* poll failed; but free the remainder scratch first */
		asm volatile(
			"omov  o1, o13\n"
			"addiu r4, r0, 0\n"
			"call  #0x101\n"          /* ObjFree (immediate) */
			"nop"
			: : : "r1", "r2", "r4"
		);
		return -6;
	}

	if (status != 0 || kind != DIR_KIND_MOUNT || rem_len <= 0) {
		/* No remainder to fetch. Free scratch. Restore ref to O1
		 * for caller's convenience (LEAF case). */
		asm volatile(
			"omov  o1, o13\n"
			"addiu r4, r0, 0\n"
			"call  #0x101\n"
			"nop"
			: : : "r1", "r2", "r4"
		);
		/* Publish the resolved ref into DIR_RESULT_SLOT so the
		 * caller can OREFLD it without depending on O1 being
		 * preserved across the function-call boundary. */
		asm volatile(
			"orefst o14, %0(o12)"
			:
			: "i"(DIR_RESULT_SLOT_OFFSET)
		);
		asm volatile("omov o1, o14");   /* also leave in O1 */
		if (kind_out) *kind_out = kind;
		return status;
	}

	/* MOUNT case: ObjFetchBytes the remainder from O13 (scratch)
	 * into the caller's remainder_buf (on stack). Source = O13,
	 * dest = O11 (boot stack), src_off=0, dst_off=stack offset of
	 * remainder_buf, count=rem_len. */
	if (rem_len + 1 > remainder_cap) {
		asm volatile(
			"omov  o1, o13\n"
			"addiu r4, r0, 0\n"
			"call  #0x101\n"
			"nop"
			: : : "r1", "r2", "r4"
		);
		asm volatile(
			"orefst o14, %0(o12)"
			:
			: "i"(DIR_RESULT_SLOT_OFFSET)
		);
		asm volatile("omov o1, o14");
		if (kind_out) *kind_out = kind;
		return -5;       /* ETOOBIG */
	}

	int fetch_status;
	asm volatile(
		"omov  o1, o13\n"             /* source = remainder scratch */
		"omov  o2, o11\n"             /* destination = boot stack */
		"addiu r4, r0, 0\n"
		"addu  r5, %1, r0\n"
		"addu  r6, %2, r0\n"
		"call  #0x108\n"              /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(fetch_status)
		: "r"(dst_offset), "r"(rem_len)
		: "r1", "r2", "r4", "r5", "r6"
	);

	/* NUL-terminate the remainder. */
	if (fetch_status == 0 && rem_len < remainder_cap) {
		remainder_buf[rem_len] = '\0';
	}

	/* Free remainder scratch. */
	asm volatile(
		"omov  o1, o13\n"
		"addiu r4, r0, 0\n"
		"call  #0x101\n"
		"nop"
		: : : "r1", "r2", "r4"
	);

	/* Publish the resolved ref into DIR_RESULT_SLOT and also
	 * leave a copy in O1 (best-effort — pcc may clobber it
	 * across the function return). Callers should OREFLD from
	 * DIR_RESULT_SLOT for reliability. */
	asm volatile(
		"orefst o14, %0(o12)"
		:
		: "i"(DIR_RESULT_SLOT_OFFSET)
	);
	asm volatile("omov o1, o14");

	if (kind_out) *kind_out = kind;
	if (fetch_status != 0) return -6;
	if (status != 0) return status;
	return rem_len;
}

/* dir_list — list children of a DIR. Same shape as dir_walk for
 * the path-packing + reply-collecting phases, but the daemon
 * writes a NUL-separated names buffer instead of a remainder. */
int
dir_list(const char *path, char *buf, int cap)
{
	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int len = dir_strlen(path);
	if (len <= 0 || len >= DIR_PATH_BUF_SIZE) return -1;
	if (cap <= 0) return -1;

	rc = dir_alloc_bytes_o14(DIR_PATH_BUF_SIZE,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (rc != 0) return rc;
	rc = dir_pack_bytes_o1(path, len, DIR_PATH_BUF_SIZE);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	int dst_offset = (int)((unsigned int)buf - STACK_BOTTOM);

	/* Allocate entries scratch in O13. */
	int alloc_status;
	asm volatile(
		"addu  r4, %1, r0\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x100\n"
		"nop\n"
		"omov  o13, o1\n"
		"addu  %0, r2, r0"
		: "=r"(alloc_status)
		: "r"(cap), "i"(TAG_DATA),
		  "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (alloc_status != 0) {
		dir_free_o14_deferred();
		return alloc_status;
	}

	/* Reply sub-cap. */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"
		"call  #0x103\n"
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(rc) : : "r1", "r2", "r4"
	);
	if (rc != 0) {
		dir_free_o14_deferred();
		asm volatile(
			"omov o1, o13\n"
			"addiu r4, r0, 0\n"
			"call #0x101\n"
			"nop"
			: : : "r1", "r2", "r4"
		);
		return rc;
	}

	asm volatile(
		"orefld o1, 584(o12)\n"
		"omov   o2, o14\n"
		"orefld o3, 608(o12)\n"
		"omov   o4, o13\n"
		"addiu  r4, r0, %0\n"
		"addu   r5, %1, r0\n"
		"addu   r6, %2, r0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(DIR_OP_LIST), "r"(len), "r"(cap)
		: "r1", "r4", "r5", "r6", "r7"
	);

	dir_free_o14_deferred();

	int status, count, bytes_written, poll_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r2, r0"
		: "=r"(status), "=r"(count), "=r"(bytes_written),
		  "=r"(poll_status)
		: : "r1", "r2", "r3", "r4", "r5"
	);
	if (poll_status != 0) {
		asm volatile(
			"omov o1, o13\n"
			"addiu r4, r0, 0\n"
			"call #0x101\n"
			"nop"
			: : : "r1", "r2", "r4"
		);
		return -6;
	}

	if (status == 0 && bytes_written > 0) {
		int fetch_status;
		asm volatile(
			"omov  o1, o13\n"
			"omov  o2, o11\n"
			"addiu r4, r0, 0\n"
			"addu  r5, %1, r0\n"
			"addu  r6, %2, r0\n"
			"call  #0x108\n"
			"nop\n"
			"addu  %0, r2, r0"
			: "=r"(fetch_status)
			: "r"(dst_offset), "r"(bytes_written)
			: "r1", "r2", "r4", "r5", "r6"
		);
		if (fetch_status != 0) status = -6;
	}

	/* Free scratch. */
	asm volatile(
		"omov o1, o13\n"
		"addiu r4, r0, 0\n"
		"call #0x101\n"
		"nop"
		: : : "r1", "r2", "r4"
	);

	if (status != 0) return status;
	return count;
}

/* dir_subscribe — Phase 54: register the notification cap currently
 * in O1 against `path` so oriscdir SENDs to it whenever the tree
 * mutates at or under that path. The notify_op (1..255) is what
 * oriscdir places in R3 of every notification — pick something
 * distinct from your other dispatch ops so your poll loop can
 * route. Returns 0 on success, negative on error.
 *
 * The caller MUST OREFLD the notify_cap into O1 immediately before
 * calling — same convention dir_register uses for its
 * ref-to-register. We stash O1 to DIR_INPUT_REF_SLOT on entry,
 * before any other code path can clobber it.
 *
 * Wire ABI (request, mirror of OP_REGISTER's shape):
 *   recipient = oriscdir mailbox (DIR_SLOT)
 *   O2 = path bytes (TAG_DATA)
 *   O3 = reply_cap (one-shot ack)
 *   O4 = notify_cap (persistent SEND target)
 *   R4 = DIR_OP_SUBSCRIBE = 6
 *   R5 = path length
 *   R6 = notify_op
 *
 * Reply: R3 = 0 OK / negative error. */
int
dir_subscribe(const char *path, int notify_op)
{
	asm volatile(
		"orefst o1, %0(o12)"
		:
		: "i"(DIR_INPUT_REF_SLOT_OFFSET)
	);

	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int len = dir_strlen(path);
	if (len <= 0 || len >= DIR_PATH_BUF_SIZE) return -1;

	rc = dir_alloc_bytes_o14(DIR_PATH_BUF_SIZE,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (rc != 0) return rc;
	rc = dir_pack_bytes_o1(path, len, DIR_PATH_BUF_SIZE);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	/* Derive R+S reply sub-cap (one-shot for the subscribe ack). */
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, 9\n"
		"call  #0x103\n"
		"nop\n"
		"orefst o1, 608(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(rc) : : "r1", "r2", "r4"
	);
	if (rc != 0) { dir_free_o14_deferred(); return rc; }

	asm volatile(
		"orefld o1, 584(o12)\n"        /* DIR_SLOT */
		"omov   o2, o14\n"
		"orefld o3, 608(o12)\n"        /* DIR_REPLY_SCRATCH */
		"orefld o4, %2(o12)\n"         /* DIR_INPUT_REF_SLOT (notify_cap) */
		"addiu  r4, r0, %0\n"
		"addu   r5, %1, r0\n"
		"addu   r6, %3, r0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		: : "i"(DIR_OP_SUBSCRIBE), "r"(len),
		    "i"(DIR_INPUT_REF_SLOT_OFFSET),
		    "r"(notify_op)
		: "r1", "r4", "r5", "r6", "r7"
	);

	dir_free_o14_deferred();

	int status, reply_status;
	asm volatile(
		"orefld o1, 552(o12)\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"
		"nop\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r2, r0"
		: "=r"(reply_status), "=r"(status)
		: : "r1", "r2", "r3", "r4"
	);
	if (status != 0) return -6;
	return reply_status;
}

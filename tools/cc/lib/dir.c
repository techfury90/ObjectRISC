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
#include "obj.h"

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
#define DIR_OP_UNREGISTER 7  /* Remove a registered leaf (inverse of
                              * REGISTER). Used by co-resident End
                              * Session teardown. Idempotent. */

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

/* (Phase 4: the old MapObject/Unmap pack helper (dir_pack_bytes_o1),
 * dir_alloc_bytes_o14, and dir_free_o14_deferred are gone — every wire op
 * now builds its request object with obj_make_bytes and frees it with
 * obj_free.) */

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

/* Restore the boot O2/O3 the obj.h SEND/poll primitives clobbered, so a
 * caller's following print etc. keeps working. (O4/self-svc is vestigial
 * on this path — see wm.c — so it's left alone.) */
static void
_dir_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

/* dir_register — bind O1 (caller-supplied) to `path` as a leaf.
 *
 * Phase 4 (foundation validation): migrated onto obj.h — proves the new
 * obj_make_bytes (TAG_DATA request object via MapObject) + obj_send_3or
 * (O2 path-bytes, O3 reply mailbox, O4 ref-to-register) keystones. The
 * other dir ops (walk/mount/list/subscribe) and dir_init / the reply
 * mailbox bootstrap stay on their proven asm for now; obj_adopt_slot
 * bridges their raw-O12-slot caps (DIR_SLOT 584, REPLY_MB 552,
 * DIR_INPUT_REF 624) into handles for this one SEND. */
int
dir_register(const char *path)
{
	/* Save the caller's O1 (the ref to register) into DIR_INPUT_REF_SLOT
	 * IMMEDIATELY — dir_init's dir_slot_isn does `orefld o1, ...` which
	 * clobbers O1. We adopt it back out of the slot below. */
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
	if (obj_init() != 0) return -6;

	/* Bridge the three caps the SEND needs out of their raw slots into
	 * handles, and build the path-bytes request object. */
	obj_t dir_h   = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t ref_h   = obj_adopt_slot(DIR_INPUT_REF_SLOT_OFFSET);
	obj_t path_h  = obj_make_bytes(path, len,
	                               CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || ref_h < 0 || path_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		obj_drop(dir_h); obj_drop(reply_h); obj_drop(ref_h);
		return -6;
	}

	/* SEND: O2 = path bytes, O3 = reply mailbox cap, O4 = ref to
	 * register; R5 = path length. */
	obj_send_3or(dir_h, path_h, reply_h, ref_h,
	             DIR_OP_REGISTER, len, 0, 0);

	/* Block on the reply. The daemon ObjFetchBytes the path before it
	 * replies, so the reply is a barrier — safe to free the path object
	 * after it (no async-buffer race). */
	int out[4];
	int prc = obj_recv_full(reply_h, out);
	_dir_restore_or();

	obj_free(path_h);              /* bytes object — daemon done with it */
	obj_drop(dir_h);               /* handles alias raw slots / caller's */
	obj_drop(reply_h);             /* ref — drop, never free */
	obj_drop(ref_h);

	if (prc != 0) return -6;
	return out[0];                 /* reply R3 = status */
}

/* dir_unregister — remove the leaf at `path` (inverse of
 * dir_register). Unlike register/mount there's no ref-to-register, so
 * no O1 to stash and no DIR_INPUT_REF_SLOT / ref_h. We just bridge the
 * dir-service and reply-mailbox caps out of their raw slots, build the
 * path-bytes request object, and SEND op=UNREGISTER. The reply doubles
 * as the barrier (the daemon ObjFetchBytes the path before replying),
 * so it's safe to free the path object afterward — no async race. */
int
dir_unregister(const char *path)
{
	int rc = dir_init();
	if (rc != 0) return rc;
	rc = dir_reply_mailbox_init();
	if (rc != 0) return rc;

	int len = dir_strlen(path);
	if (len <= 0 || len >= DIR_PATH_BUF_SIZE) return -1;
	if (obj_init() != 0) return -6;

	obj_t dir_h   = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t path_h  = obj_make_bytes(path, len,
	                               CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || path_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		obj_drop(dir_h); obj_drop(reply_h);
		return -6;
	}

	/* SEND: O2 = path bytes, O3 = reply mailbox cap, O4 = null
	 * (no ref to register); R5 = path length. */
	obj_send_3or(dir_h, path_h, reply_h, OBJ_NULL,
	             DIR_OP_UNREGISTER, len, 0, 0);

	int out[4];
	int prc = obj_recv_full(reply_h, out);
	_dir_restore_or();

	obj_free(path_h);              /* bytes object — daemon done with it */
	obj_drop(dir_h);               /* handles alias raw slots / caller's */
	obj_drop(reply_h);             /* ref — drop, never free */

	if (prc != 0) return -6;
	return out[0];                 /* reply R3 = status */
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

	if (obj_init() != 0) return -6;

	/* The daemon wants path\0prefix\0 in one bytes object. Build it on the
	 * stack, then obj_make_bytes copies the whole thing (embedded NULs
	 * included) into a fresh TAG_DATA object at offset 0. */
	char buf[DIR_PATH_BUF_SIZE];
	int i, n = 0;
	for (i = 0; i < plen; i++) buf[n++] = path[i];
	buf[n++] = '\0';
	for (i = 0; i < xlen; i++) buf[n++] = prefix[i];
	buf[n++] = '\0';

	obj_t dir_h   = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t ref_h   = obj_adopt_slot(DIR_INPUT_REF_SLOT_OFFSET);
	obj_t path_h  = obj_make_bytes(buf, n, CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || ref_h < 0 || path_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		obj_drop(dir_h); obj_drop(reply_h); obj_drop(ref_h);
		return -6;
	}

	/* O2 = path\0prefix\0 bytes, O3 = reply mailbox, O4 = service ref;
	 * R5 = path length, R6 = prefix length. */
	obj_send_3or(dir_h, path_h, reply_h, ref_h,
	             DIR_OP_MOUNT, plen, xlen, 0);

	int out[4];
	int prc = obj_recv_full(reply_h, out);
	_dir_restore_or();

	obj_free(path_h);
	obj_drop(dir_h); obj_drop(reply_h); obj_drop(ref_h);

	if (prc != 0) return -6;
	return out[0];
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
	if (obj_init() != 0) return -6;

	/* The daemon writes the MOUNT remainder into the O4 object; we
	 * ObjFetchBytes it into remainder_buf (on the boot stack) afterward,
	 * at this offset. */
	int dst_offset = (int)((unsigned int)remainder_buf - STACK_BOTTOM);

	obj_t dir_h   = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t path_h  = obj_make_bytes(path, len, CAP_R | CAP_W | CAP_V | CAP_C);
	obj_t rem_h   = obj_alloc(DIR_REM_BUF_SIZE, OBJ_TAG_DATA,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || path_h < 0 || rem_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		if (rem_h >= 0)  obj_free(rem_h);
		obj_drop(dir_h); obj_drop(reply_h);
		return -6;
	}

	/* O2 = path bytes, O3 = reply mailbox, O4 = remainder scratch;
	 * R5 = path length, R6 = remainder capacity. */
	obj_send_3or(dir_h, path_h, reply_h, rem_h,
	             DIR_OP_WALK, len, DIR_REM_BUF_SIZE, 0);
	/* The service cap is dead the moment the SEND returns; drop it now so
	 * its handle slot is free for obj_recv_cap_full's resolved-ref handle.
	 * Otherwise dir_walk's peak is 5 simultaneous handles (dir+reply+path+
	 * rem+resolved); with this drop it is 4. Headroom under the 16-slot
	 * table (OBJ_NHANDLE), and it was a hard overflow back when the table
	 * was 8 (the shell holds several persistent handles of its own). */
	obj_drop(dir_h);

	/* Reply: R3=status, R4=kind, R5=rem_len, O2=resolved ref (null for a
	 * plain directory). Receive the cap into a handle, mirror it into
	 * DIR_RESULT_SLOT for callers (obj_adopt_dir_result reads it), drop. */
	int rep[4];
	obj_t resolved_h = obj_recv_cap_full(reply_h, rep);
	_dir_restore_or();

	obj_free(path_h);
	obj_drop(reply_h);

	if (resolved_h < 0) {              /* poll itself failed */
		obj_free(rem_h);
		return -6;
	}
	obj_park_dir_result(resolved_h);   /* resolved ref (or null) -> 616 */
	obj_drop(resolved_h);

	int status  = rep[0];
	int kind    = rep[1];
	int rem_len = rep[2];
	if (kind_out) *kind_out = kind;

	if (status != 0 || kind != DIR_KIND_MOUNT || rem_len <= 0) {
		obj_free(rem_h);               /* no remainder to fetch */
		return status;
	}
	if (rem_len + 1 > remainder_cap) {
		obj_free(rem_h);
		return -5;                     /* ETOOBIG */
	}

	/* MOUNT: copy the daemon-written remainder out of the scratch object
	 * into the caller's stack buffer. */
	int fetch_status = obj_fetch_to_stack(rem_h, dst_offset, rem_len);
	obj_free(rem_h);
	_dir_restore_or();
	if (fetch_status == 0 && rem_len < remainder_cap)
		remainder_buf[rem_len] = '\0';
	if (fetch_status != 0) return -6;
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
	if (obj_init() != 0) return -6;

	int dst_offset = (int)((unsigned int)buf - STACK_BOTTOM);

	obj_t dir_h   = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t path_h  = obj_make_bytes(path, len, CAP_R | CAP_W | CAP_V | CAP_C);
	obj_t ent_h   = obj_alloc(cap, OBJ_TAG_DATA,
	                          CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || path_h < 0 || ent_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		if (ent_h >= 0)  obj_free(ent_h);
		obj_drop(dir_h); obj_drop(reply_h);
		return -6;
	}

	/* O2 = path bytes, O3 = reply mailbox, O4 = entries scratch (the
	 * daemon writes the NUL-separated names into it); R5 = path length,
	 * R6 = buffer capacity. */
	obj_send_3or(dir_h, path_h, reply_h, ent_h,
	             DIR_OP_LIST, len, cap, 0);
	obj_drop(dir_h);                  /* service cap dead after the SEND */

	/* Reply: R3=status, R4=count, R5=bytes_written. */
	int rep[4];
	int prc = obj_recv_full(reply_h, rep);
	_dir_restore_or();

	obj_free(path_h);
	obj_drop(reply_h);

	if (prc != 0) { obj_free(ent_h); return -6; }
	int status = rep[0];
	int count  = rep[1];
	int bytes_written = rep[2];
	if (status == 0 && bytes_written > 0) {
		int fs = obj_fetch_to_stack(ent_h, dst_offset, bytes_written);
		_dir_restore_or();
		if (fs != 0) status = -6;
	}
	obj_free(ent_h);

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
	/* Save the caller's O1 (the notify cap to register) into
	 * DIR_INPUT_REF_SLOT immediately — dir_init clobbers O1. */
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
	if (obj_init() != 0) return -6;

	obj_t dir_h    = obj_adopt_slot(DIR_SLOT_OFFSET);
	obj_t reply_h  = obj_adopt_slot(REPLY_MB_SLOT_OFFSET);
	obj_t notify_h = obj_adopt_slot(DIR_INPUT_REF_SLOT_OFFSET);
	obj_t path_h   = obj_make_bytes(path, len, CAP_R | CAP_W | CAP_V | CAP_C);
	if (dir_h < 0 || reply_h < 0 || notify_h < 0 || path_h < 0) {
		if (path_h >= 0) obj_free(path_h);
		obj_drop(dir_h); obj_drop(reply_h); obj_drop(notify_h);
		return -6;
	}

	/* O2 = path bytes, O3 = reply mailbox, O4 = the persistent notify
	 * cap oriscdir will SEND mutations to; R5 = path length,
	 * R6 = notify_op. */
	obj_send_3or(dir_h, path_h, reply_h, notify_h,
	             DIR_OP_SUBSCRIBE, len, notify_op, 0);

	int out[4];
	int prc = obj_recv_full(reply_h, out);
	_dir_restore_or();

	obj_free(path_h);
	obj_drop(dir_h); obj_drop(reply_h); obj_drop(notify_h);

	if (prc != 0) return -6;
	return out[0];
}

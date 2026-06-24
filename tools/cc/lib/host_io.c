/*
 * host_io.c — Object RISC libc: host filesystem I/O via hostfsd.
 *
 * Wraps the wire protocol documented in tools/devices/hostfsd. Each
 * function SENDs a request to the hostfsd service and blocks on a
 * private receive queue for the response.
 *
 * Phase 4: migrated onto the handle-based object API (obj.h). The two
 * capabilities this client needs are now `obj_t` handles in the O12
 * handle table instead of hand-held boot OPRs:
 *
 *   - the hostfsd service, adopted from boot register O10
 *     (obj_adopt_o10), and
 *   - a private 16-byte reply mailbox, allocated + queue-attached in
 *     the handle table.
 *
 * Every request is one obj_send_bytes(hostfsd, src, mbox, op, …): O2 =
 * the source segment ref (stack/data/none) so hostfsd can ObjFetchBytes
 * the path/buffer, O3 = the reply mailbox cap. hf_wait then blocks on
 * the mailbox via obj_recv_full. Because each helper blocks on its
 * reply, hostfsd has already fetched (or written) the buffer by the time
 * the reply lands — so host_io is immune to the async-buffer-lifetime
 * trap that fire-and-forget clients (grid/raster) must guard against.
 *
 * The O8 mirror
 * -------------
 * The mailbox cap is ALSO mirrored into boot register O8 (obj_park_o8)
 * because two consumers still read it directly: term.c's
 * term_print_n_sync derives its reply-cap from O8 and blocks on O8 for
 * the console ack (cmd_cat / cmd_ls / cmd_more serialize hf_read with it
 * so the queue holds at most one outstanding message), and the
 * supervisor's orx_task_create harvests a child's O8 around TaskCreate.
 * Once those migrate too, the mirror can go.
 *
 * OR-hygiene convention REQUIRED of callers
 * -----------------------------------------
 *     O8  = hf mailbox cap     (mirrored by hf_init; see above)
 *     O10 = hostfsd service    (caller arranges via --service order;
 *                               adopted into a handle by hf_init)
 *     O11 = boot stack ref      (for hf_read destination buffers)
 *     O14 = boot self-svc       (restored on every helper exit)
 *     O15 = boot data ref       (for hf_write source buffers + libc
 *                                console output)
 *
 * The object API uses O2/O3/O4 as scratch, so each helper restores them
 * from O11/O15/O14 on the way out (matching the discipline
 * mouse_paint.c established) so callers' print_str etc. keep working
 * after every hf_* call.
 *
 * hf_init() does the subscribe handshake with hostfsd. Call it once
 * before any other hf_* call. Because its mailbox now lives in the
 * obj.h handle table (in the O12 task table), task_init() MUST have run
 * first — the universal boot order every hostfs-using program already
 * follows (shell, supervisor, login, edit, host_cat).
 */

#include "liborisc.h"
#include "obj.h"

/* Wire-protocol op codes — must match tools/devices/hostfsd. */
#define OP_OPEN       0
#define OP_CLOSE      1
#define OP_READ       2
#define OP_WRITE      3
#define OP_SUBSCRIBE  4
#define OP_OPENDIR    5
#define OP_MKDIR      6
#define OP_UNLINK     7

/* Section base VAs from CONTRACT.md §2. */
#define DATA_VA  0x00040000
#define STACK_TOP 0x00200000

/* The default stack is 64 KiB (CONTRACT.md §2). The bottom of the
 * stack object is therefore STACK_TOP - 0x10000 = 0x001f0000. */
#define STACK_BOTTOM 0x001f0000

/* Mailbox caps: R|W|S|V|C (== 0x5b). The C bit is the derive-rights bit
 * ObjDerive needs to cut the R|S subscriber sub-cap below. */
#define HF_MBOX_CAPS \
	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

/* The hostfsd service (adopted from O10) and our private reply mailbox,
 * both as object handles. */
static obj_t hostfsd_h = OBJ_NULL;
static obj_t hf_mbox_h = OBJ_NULL;

/* --- helpers shared by all ops ----------------------------------------- */

static void
hf_restore_or(void)
{
	/* Bring back O2/O3/O4 from the caller's boot-saved slots. */
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

static int
hf_strlen(const char *s)
{
	const char *p;
	for (p = s; *p; p++) ;
	return (int)(p - s);
}

/* Resolve a buffer VA to (segment, byte offset) for obj_send_bytes:
 * stack buffers ride O11, data/static buffers ride O15. */
static int
hf_seg_off(const void *buf, int *off)
{
	unsigned int va = (unsigned int)buf;
	if (va >= STACK_BOTTOM && va < STACK_TOP) {
		*off = (int)(va - STACK_BOTTOM);
		return OBJ_SRC_STACK;
	}
	*off = (int)(va - DATA_VA);
	return OBJ_SRC_DATA;
}

/* Block on the hf mailbox for one response from hostfsd. The response
 * carries (primary in R3, secondary in R4) — see the protocol table in
 * tools/devices/hostfsd. The primary is what the user-facing function
 * returns; secondary is the file size hf_open receives alongside the
 * fd. */
static int
hf_wait(int *out_secondary)
{
	int out[4];
	int rc = obj_recv_full(hf_mbox_h, out);   /* blocks; full R3..R6 */
	hf_restore_or();
	if (rc != 0) {
		if (out_secondary) *out_secondary = 0;
		return -1;     /* poll itself failed; treat as bad */
	}
	if (out_secondary) *out_secondary = out[1];
	return out[0];
}

/* --- hf_init — adopt hostfsd, allocate mailbox, subscribe -------------- */

int
hf_init(void)
{
	obj_t sub;

	if (obj_init() != 0)
		return -1;

	/* Adopt the boot hostfsd-service cap (O10) into a handle. */
	hostfsd_h = obj_adopt_o10();
	if (hostfsd_h < 0)
		return -1;

	/* Private 16-byte reply mailbox + depth-16 queue. Without it,
	 * hostfsd responses would land on the boot self-svc queue
	 * alongside keyboard events — a long cat would then dequeue a
	 * keystroke as a read reply and mis-decode the count. */
	hf_mbox_h = obj_alloc(16, OBJ_TAG_SERVICE, HF_MBOX_CAPS);
	if (hf_mbox_h < 0)
		return -1;
	if (obj_queue_attach(hf_mbox_h, 16) != 0)
		return -1;

	/* Mirror the mailbox cap into O8 for the legacy direct-O8
	 * consumers (term_print_n_sync, supervisor) — see file header. */
	obj_park_o8(hf_mbox_h);

	/* Derive an R|S sub-cap of the mailbox and SEND it to hostfsd as
	 * OP_SUBSCRIBE (O2 = sub-cap). hostfsd keeps its own copy, so we
	 * drop ours. */
	sub = obj_derive(hf_mbox_h, OBJ_CAP_R | OBJ_CAP_S);
	if (sub < 0)
		return -1;
	obj_send_or(hostfsd_h, sub, OP_SUBSCRIBE, 0, 0, 0);
	obj_drop(sub);
	hf_restore_or();
	return 0;
}

/* --- hf_open ----------------------------------------------------------- */

int
hf_open(const char *path, int flags)
{
	int off, src, discard;

	src = hf_seg_off(path, &off);
	obj_send_bytes(hostfsd_h, src, hf_mbox_h,
	               OP_OPEN, off, hf_strlen(path), flags);
	return hf_wait(&discard);     /* secondary = file size; ignored */
}

/* --- hf_opendir — open a directory; subsequent hf_read returns a
 *     "name1\nname2\n..." listing of the entries (subdirs end "/"). */

int
hf_opendir(const char *path)
{
	int off, src, discard;

	src = hf_seg_off(path, &off);
	obj_send_bytes(hostfsd_h, src, hf_mbox_h,
	               OP_OPENDIR, off, hf_strlen(path), 0);
	return hf_wait(&discard);
}

/* --- hf_mkdir — Phase 50: create a directory at `path`. Returns 0 on
 *     success, negative errno on failure (E_EXIST if the entry already
 *     exists, E_NOENT if a parent component is missing). */

int
hf_mkdir(const char *path)
{
	int off, src, discard;

	src = hf_seg_off(path, &off);
	obj_send_bytes(hostfsd_h, src, hf_mbox_h,
	               OP_MKDIR, off, hf_strlen(path), 0);
	return hf_wait(&discard);
}

/* --- hf_unlink — Phase 50: remove the file at `path`. Returns 0 on
 *     success, negative errno (E_NOENT if missing, E_EXIST when the
 *     target is a directory — POSIX-style "use rmdir for that"). */

int
hf_unlink(const char *path)
{
	int off, src, discard;

	src = hf_seg_off(path, &off);
	obj_send_bytes(hostfsd_h, src, hf_mbox_h,
	               OP_UNLINK, off, hf_strlen(path), 0);
	return hf_wait(&discard);
}

/* --- hf_close ---------------------------------------------------------- */

int
hf_close(int fd)
{
	int discard;

	obj_send_bytes(hostfsd_h, OBJ_SRC_NONE, hf_mbox_h,
	               OP_CLOSE, fd, 0, 0);
	return hf_wait(&discard);
}

/* --- hf_read — buffer must be on the stack (hostfsd needs W cap) ------- */

int
hf_read(int fd, char *buf, int count)
{
	int off, discard;

	if ((unsigned int)buf < STACK_BOTTOM
	    || (unsigned int)buf >= STACK_TOP) {
		/* Data-segment buffers can't be a host-read destination: the
		 * data ref has no W cap. Caller must use a stack buffer. */
		return -2;
	}
	off = (int)((unsigned int)buf - STACK_BOTTOM);
	obj_send_bytes(hostfsd_h, OBJ_SRC_STACK, hf_mbox_h,
	               OP_READ, fd, off, count);
	return hf_wait(&discard);
}

/* --- hf_write — buffer can be in stack or data ------------------------- */

int
hf_write(int fd, const char *buf, int count)
{
	int off, src, discard;

	src = hf_seg_off(buf, &off);
	obj_send_bytes(hostfsd_h, src, hf_mbox_h,
	               OP_WRITE, fd, off, count);
	return hf_wait(&discard);
}

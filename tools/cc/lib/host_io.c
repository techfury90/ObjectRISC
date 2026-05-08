/*
 * host_io.c — Object RISC libc: host filesystem I/O via hostfsd.
 *
 * Wraps the wire protocol documented in tools/devices/hostfsd. Each
 * function SENDs a request to the hostfsd service object (passed in
 * O10 by convention — see hf_init below) and blocks on a private
 * receive queue for the response.
 *
 * The per-service mailbox
 * -----------------------
 * hf_init() ObjAllocs its own 16-byte mailbox, attaches a depth-16
 * queue, derives an R|S sub-ref to hand to hostfsd as the
 * subscription target, and parks the full ref in O8. All hostfsd
 * responses land on that mailbox's queue; hf_wait() polls it.
 *
 * Why a private mailbox: term_init's queue on O4 (boot self-svc) is
 * shared by keyboard events, so a long-running cat would otherwise
 * dequeue a keystroke instead of a hostfsd reply and mis-decode the
 * R3 field. Same shape as lb_init's mailbox in linkboot.c.
 *
 * OR-hygiene convention REQUIRED of callers
 * -----------------------------------------
 * These functions assume the program has parked the boot-time refs
 * in known slots:
 *
 *     O8  = hf mailbox full ref (parked by hf_init)
 *     O11 = boot stack ref      (for hf_read destination buffers)
 *     O14 = boot self-svc       (restored on every helper exit)
 *     O15 = boot data ref       (for hf_write source buffers + libc
 *                                console output)
 *
 * Each helper restores O2/O3/O4 from O11/O15/O14 on the way out
 * (matching the discipline mouse_paint.c established), so callers'
 * print_str etc. keep working after every hf_* call.
 *
 * hf_init() does the subscribe handshake with hostfsd. Call it once
 * before any other hf_* call. The boot ABI puts hostfsd's service
 * ref in O10 if the runner script's --service order matches the
 * convention; the program is responsible for getting it there.
 */

#include "liborisc.h"

/* Wire-protocol constants — must match tools/devices/hostfsd. */
#define OP_OPEN       0
#define OP_CLOSE      1
#define OP_READ       2
#define OP_WRITE      3
#define OP_SUBSCRIBE  4
#define OP_OPENDIR    5

/* Section base VAs from CONTRACT.md §2. */
#define DATA_VA  0x00040000
#define STACK_TOP 0x00200000

/* The default stack is 64 KiB (CONTRACT.md §2). The bottom of the
 * stack object is therefore STACK_TOP - 0x10000 = 0x001f0000. */
#define STACK_BOTTOM 0x001f0000

/* Reserved OR slots for hostfsd-using programs (see top-of-file). */
/*   O8  — hf mailbox full ref (parked by hf_init; poll target) */
/*   O10 — hostfsd service ref (caller arranges via --service order) */
/*   O11 — boot stack ref (writable; for hf_read destinations) */
/*   O14 — boot self-svc (restored on exit) */
/*   O15 — boot data ref (read-only; for hf_write sources) */

/* --- helpers shared by all ops ----------------------------------------- */

static void
hf_restore_or(void)
{
	/* Bring back O2/O3/O4 from caller's saved slots. Same shape as
	 * mouse_paint.c::restore_or_state. */
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

/* Block on the hf mailbox for one response from hostfsd. The
 * response carries (primary in R3, secondary in R4) — see the
 * protocol table in tools/devices/hostfsd. Caller's primary value
 * is what the user-facing function returns; secondary is used by
 * hf_open to receive the file size alongside the fd. */
static int
hf_wait(int *out_secondary)
{
	int status, primary, secondary;
	asm volatile(
		"omov  o1, o8\n"            /* hf mailbox (private queue) */
		"addiu r4, r0, -1\n"
		"call  #0x204\n"            /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"        /* status */
		"addu  %1, r3, r0\n"        /* primary value */
		"addu  %2, r4, r0"          /* secondary */
		: "=r"(status), "=r"(primary), "=r"(secondary)
		:
		: "r1", "r2", "r3", "r4"
	);
	hf_restore_or();
	if (status != 0) {
		if (out_secondary) *out_secondary = 0;
		return -1;     /* poll itself failed; treat as bad */
	}
	if (out_secondary) *out_secondary = secondary;
	return primary;
}

/* --- hf_init — allocate mailbox + subscribe to hostfsd ----------------- *
 *
 * ObjAllocs a private 16-byte mailbox (TAG_SERVICE-typed),
 * attaches a depth-16 queue, derives an R|S sub-ref to hand
 * hostfsd, and parks the full ref in O8 for later polls. Without
 * the private mailbox, hostfsd responses would land on the boot
 * self-svc queue alongside keyboard events — a long cat then
 * dequeues a keystroke as if it were a read reply and mis-decodes
 * the count. */

int
hf_init(void)
{
	int status;

	/* ObjAlloc(16, TAG_SERVICE, R|W|S|V|C). */
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, 0x4103\n"     /* TAG_SERVICE */
		"addiu r6, r0, 0x5b\n"        /* R|W|S|V|C */
		"addiu r7, r0, 0\n"
		"call  #0x100\n"              /* ObjAlloc → O1 */
		"nop\n"
		"addu  %0, r2, r0\n"
		"omov  o8, o1"                /* o8 = mailbox full ref */
		: "=r"(status)
		:
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) {
		hf_restore_or();
		return -1;
	}

	/* Attach a queue to the mailbox. */
	asm volatile(
		"omov  o1, o8\n"
		"addiu r4, r0, 16\n"
		"call  #0x203\n"              /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r4"
	);
	if (status != 0) {
		hf_restore_or();
		return -1;
	}

	/* Derive an R|S sub-ref from the mailbox and SEND it to
	 * hostfsd as OP_SUBSCRIBE. We move the derived ref straight
	 * into O2 (the SEND's source slot) without parking it in O9 —
	 * O9 is now claimed by term_init for its own kbd queue, and
	 * stomping on it here would lose the editor's mailbox. */
	asm volatile(
		"omov  o1, o8\n"
		"addiu r4, r0, 9\n"           /* R|S */
		"call  #0x103\n"              /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"omov  o2, o1\n"              /* O2 = sub-cap for SEND */
		"omov  o1, o10\n"             /* O1 = hostfsd */
		"onull o3\n"
		"addiu r4, r0, 4\n"          /* OP_SUBSCRIBE */
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		:
		: "r1", "r4", "r5", "r6", "r7"
	);
	hf_restore_or();
	return 0;
}

/* --- hf_open ----------------------------------------------------------- */

int
hf_open(const char *path, int flags)
{
	int path_off, path_len, fd;
	const char *p;

	/* Path lives in our data segment (string literal) or possibly in
	 * the stack — figure out which by VA range. The lib only reads
	 * from the path so either OR will do (R cap suffices). */
	for (p = path, path_len = 0; *p; p++, path_len++) ;
	if ((unsigned int)path >= STACK_BOTTOM
	    && (unsigned int)path < STACK_TOP) {
		path_off = (int)((unsigned int)path - STACK_BOTTOM);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o11\n"            /* stack ref */
			"omov  o3, o8\n"
			"addiu r4, r0, 0\n"          /* OP_OPEN */
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addu  r7, %2, r0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len), "r"(flags)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		path_off = (int)((unsigned int)path - DATA_VA);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o15\n"            /* data ref */
			"omov  o3, o8\n"
			"addiu r4, r0, 0\n"
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addu  r7, %2, r0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len), "r"(flags)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	hf_restore_or();
	{
		int discard;
		fd = hf_wait(&discard);   /* secondary = file size; ignored for now */
	}
	return fd;
}

/* --- hf_opendir — open a directory; subsequent hf_read returns
 *     a "name1\nname2\n..." listing of the entries (subdirs end "/"). */

int
hf_opendir(const char *path)
{
	int path_off, path_len, fd;
	const char *p;
	for (p = path, path_len = 0; *p; p++, path_len++) ;
	if ((unsigned int)path >= STACK_BOTTOM
	    && (unsigned int)path < STACK_TOP) {
		path_off = (int)((unsigned int)path - STACK_BOTTOM);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o11\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 5\n"          /* OP_OPENDIR */
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		path_off = (int)((unsigned int)path - DATA_VA);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o15\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 5\n"
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	hf_restore_or();
	{
		int discard;
		fd = hf_wait(&discard);
	}
	return fd;
}

/* --- hf_mkdir — Phase 50: create a directory at `path`. Returns 0
 *     on success, negative errno on failure (E_EXIST if the entry
 *     already exists, E_NOENT if a parent component is missing).
 *     Same wire shape as hf_opendir minus the fd-return. */

int
hf_mkdir(const char *path)
{
	int path_off, path_len;
	const char *p;
	for (p = path, path_len = 0; *p; p++, path_len++) ;
	if ((unsigned int)path >= STACK_BOTTOM
	    && (unsigned int)path < STACK_TOP) {
		path_off = (int)((unsigned int)path - STACK_BOTTOM);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o11\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 6\n"          /* OP_MKDIR */
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		path_off = (int)((unsigned int)path - DATA_VA);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o15\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 6\n"
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	hf_restore_or();
	{
		int discard;
		return hf_wait(&discard);
	}
}

/* --- hf_unlink — Phase 50: remove the file at `path`. Returns 0 on
 *     success, negative errno (E_NOENT if missing, E_EXIST when the
 *     target is a directory — POSIX-style "use rmdir for that").
 *     Same shape as hf_mkdir. */

int
hf_unlink(const char *path)
{
	int path_off, path_len;
	const char *p;
	for (p = path, path_len = 0; *p; p++, path_len++) ;
	if ((unsigned int)path >= STACK_BOTTOM
	    && (unsigned int)path < STACK_TOP) {
		path_off = (int)((unsigned int)path - STACK_BOTTOM);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o11\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 7\n"          /* OP_UNLINK */
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		path_off = (int)((unsigned int)path - DATA_VA);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o15\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 7\n"
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	hf_restore_or();
	{
		int discard;
		return hf_wait(&discard);
	}
}

/* --- hf_close ---------------------------------------------------------- */

int
hf_close(int fd)
{
	asm volatile(
		"omov  o1, o10\n"
		"onull o2\n"
		"omov  o3, o8\n"
		"addiu r4, r0, 1\n"            /* OP_CLOSE */
		"addu  r5, %0, r0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "r"(fd)
		: "r1", "r4", "r5", "r6", "r7"
	);
	hf_restore_or();
	{
		int discard;
		return hf_wait(&discard);
	}
}

/* --- hf_read — buffer must be on the stack (hostfsd needs W cap) ------- */

int
hf_read(int fd, char *buf, int count)
{
	int buf_off;
	if ((unsigned int)buf < STACK_BOTTOM
	    || (unsigned int)buf >= STACK_TOP) {
		/* Data-segment buffers can't be the destination of a host
		 * read because the data ref has no W cap. Caller must use
		 * a stack-allocated buffer. */
		return -2;
	}
	buf_off = (int)((unsigned int)buf - STACK_BOTTOM);
	asm volatile(
		"omov  o1, o10\n"
		"omov  o2, o11\n"              /* stack ref (R|W) */
		"omov  o3, o8\n"
		"addiu r4, r0, 2\n"            /* OP_READ */
		"addu  r5, %0, r0\n"
		"addu  r6, %1, r0\n"
		"addu  r7, %2, r0\n"
		"send  o1"
		:
		: "r"(fd), "r"(buf_off), "r"(count)
		: "r1", "r4", "r5", "r6", "r7"
	);
	hf_restore_or();
	{
		int discard;
		return hf_wait(&discard);
	}
}

/* --- hf_write — buffer can be in stack or data ------------------------- */

int
hf_write(int fd, const char *buf, int count)
{
	int buf_off;
	if ((unsigned int)buf >= STACK_BOTTOM
	    && (unsigned int)buf < STACK_TOP) {
		buf_off = (int)((unsigned int)buf - STACK_BOTTOM);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o11\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 3\n"        /* OP_WRITE */
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addu  r7, %2, r0\n"
			"send  o1"
			:
			: "r"(fd), "r"(buf_off), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		buf_off = (int)((unsigned int)buf - DATA_VA);
		asm volatile(
			"omov  o1, o10\n"
			"omov  o2, o15\n"
			"omov  o3, o8\n"
			"addiu r4, r0, 3\n"
			"addu  r5, %0, r0\n"
			"addu  r6, %1, r0\n"
			"addu  r7, %2, r0\n"
			"send  o1"
			:
			: "r"(fd), "r"(buf_off), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	hf_restore_or();
	{
		int discard;
		return hf_wait(&discard);
	}
}

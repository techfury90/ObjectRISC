/*
 * linkboot.c — Object RISC libc: client side of the linkbootd
 * spawn protocol.
 *
 * Lets a program (the shell) ask linkbootd to load and run another
 * .orx by path. Each spawn:
 *
 *   1. lb_spawn(path) builds a SEND to linkbootd's service ref
 *      carrying { O2 = path source, O3 = our R+S mailbox-ref,
 *                R4 = path_offset, R5 = path_length, R6 = OP_SPAWN }.
 *   2. linkbootd OBJ_READs the path, loads the .orx, queues it for
 *      the next idle pool-CPU loader, and serves it 256 B at a time
 *      (chunkboot.s on the receiving side; protocol described in
 *      examples/linkboot/gen_chunkboot.py).
 *   3. The guest runs to TaskExit; simorisc --reset-on-exit wipes the
 *      pool CPU and re-runs the loader; the loader's fresh announce
 *      tells linkbootd "previous run finished".
 *   4. linkbootd SENDs a result message to our mailbox-ref carrying
 *      { R3 = LB_RESULT_MAGIC, R4 = OP_SPAWN, R5 = exit_code }.
 *
 * Boot-ABI convention assumed by these helpers:
 *
 *     O7  = linkbootd service  (caller arranges via --service order;
 *                               replaces the first "pad" slot in the
 *                               shell's standard layout)
 *     O11 = boot stack ref     (parked by term_init for path sources)
 *     O14 = boot self-svc      (queue target, restored on exit)
 *     O15 = boot data ref      (path sources from .data)
 *
 * lb_init() ObjAllocs a 16-byte "mailbox" object (TAG_SERVICE-typed),
 * attaches a depth-4 receive queue to it, and parks the full ref in
 * O12 / a derived R|S sub-ref in O13. lb_spawn() polls O12's queue
 * for the result; this keeps spawn responses out of the main self-svc
 * queue where keyboard events live, so a key the user types during
 * the spawn doesn't get mistakenly consumed as a spawn result.
 *
 * Why O12/O13: the OR file only has O0..O15. O5, O6, O10 are taken
 * by terminal/hostfsd refs; O11/O14/O15 by term_init parking; O7..O9
 * are nominal "pad" slots (one of which now holds linkbootd). That
 * leaves O12/O13 unused — perfect for the mailbox.
 */

#include "liborisc.h"

/* Section base VAs from CONTRACT.md §2. */
#define DATA_VA       0x00040000
#define STACK_BOTTOM  0x001f0000
#define STACK_TOP     0x00200000

/* Wire constants — must agree with tools/devices/linkbootd. */
#define OP_SPAWN          1
#define LB_RESULT_MAGIC   0x4C425251

/* Object type tag for the mailbox. SERVICE is the closest fit
 * semantically (we attach a receive queue to it). */
#define TAG_SERVICE 0x4103

/* --- helper: restore the libc-required OR slots after a CALL ---------- */

static void
_lb_restore_or(void)
{
	/* Match host_io.c::hf_restore_or — bring back O2/O3/O4 from
	 * caller's saved slots. */
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

/* --- lb_init ----------------------------------------------------------- */

int
lb_init(void)
{
	int status;

	/* ObjAlloc a small mailbox. Caps R|W|S|V|C lets us SEND-receive
	 * (S), poll the queue (V), and derive an R|S sub-ref to give
	 * linkbootd (C). Length 16 — we never write to it. */
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, 0x4103\n"          /* TAG_SERVICE */
		"addiu r6, r0, 0x5b\n"            /* R|W|S|V|C */
		"addiu r7, r0, 0\n"
		"call  #0x100\n"                  /* ObjAlloc → O1 */
		"nop\n"
		"addu  %0, r2, r0\n"
		"omov  o12, o1"                   /* o12 = mailbox full ref */
		: "=r"(status)
		:
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) {
		_lb_restore_or();
		return -1;
	}

	/* Attach a receive queue (depth 4 — we only ever wait for one
	 * spawn-result at a time). */
	asm volatile(
		"omov  o1, o12\n"
		"addiu r4, r0, 4\n"
		"call  #0x203\n"                  /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r4"
	);
	if (status != 0) {
		_lb_restore_or();
		return -1;
	}

	/* Derive an R|S sub-ref for linkbootd. */
	asm volatile(
		"omov  o1, o12\n"
		"addiu r4, r0, 0x09\n"            /* R|S */
		"call  #0x103\n"                  /* ObjDerive → O1 */
		"nop\n"
		"addu  %0, r2, r0\n"
		"omov  o13, o1"                   /* o13 = R|S ref for linkbootd */
		: "=r"(status)
		:
		: "r1", "r2", "r4"
	);
	_lb_restore_or();
	return status == 0 ? 0 : -1;
}

/* --- internal: SEND the spawn request --------------------------------- */

static void
_lb_send_spawn(int path_off, int path_len, int from_stack)
{
	if (from_stack) {
		asm volatile(
			"omov  o1, o7\n"             /* recipient = linkbootd */
			"omov  o2, o11\n"             /* path source = stack */
			"omov  o3, o13\n"             /* mailbox R|S */
			"onull o4\n"
			"addu  r4, %0, r0\n"          /* path_off */
			"addu  r5, %1, r0\n"          /* path_len */
			"addiu r6, r0, 1\n"           /* OP_SPAWN */
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		asm volatile(
			"omov  o1, o7\n"
			"omov  o2, o15\n"             /* path source = data */
			"omov  o3, o13\n"
			"onull o4\n"
			"addu  r4, %0, r0\n"
			"addu  r5, %1, r0\n"
			"addiu r6, r0, 1\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(path_off), "r"(path_len)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	_lb_restore_or();
}

/* --- internal: block on the mailbox queue until a result arrives ------ *
 *
 * Returns the exit code (R5 of the result message), or -1 on poll
 * failure. We only ever expect one kind of message on this queue —
 * the linkbootd spawn-result. If R3 != LB_RESULT_MAGIC the message
 * is unexpected; loop and keep polling. */

static int
_lb_wait_result(void)
{
	int status, magic, op, code;

	while (1) {
		asm volatile(
			"omov  o1, o12\n"             /* mailbox */
			"addiu r4, r0, -1\n"          /* infinite */
			"call  #0x204\n"              /* ReceiveQueuePoll */
			"nop\n"
			"addu  %0, r2, r0\n"
			"addu  %1, r3, r0\n"
			"addu  %2, r4, r0\n"
			"addu  %3, r5, r0"
			: "=r"(status), "=r"(magic), "=r"(op), "=r"(code)
			:
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);
		_lb_restore_or();
		if (status != 0) return -1;
		if (magic != LB_RESULT_MAGIC) {
			/* Stray message in the mailbox — shouldn't happen unless
			 * something else SENDs to our R|S mailbox-ref. Discard
			 * and keep polling. */
			continue;
		}
		(void)op;
		return code;
	}
}

/* --- lb_spawn ---------------------------------------------------------- */

int
lb_spawn(const char *path)
{
	int path_len;
	int path_off;
	const char *p;
	unsigned int va = (unsigned int)path;

	/* Walk for length (no strlen call — keeps the OR clobber set
	 * tight). */
	for (p = path, path_len = 0; *p; p++, path_len++) ;

	if (va >= STACK_BOTTOM && va < STACK_TOP) {
		path_off = (int)(va - STACK_BOTTOM);
		_lb_send_spawn(path_off, path_len, 1);
	} else {
		path_off = (int)(va - DATA_VA);
		_lb_send_spawn(path_off, path_len, 0);
	}

	return _lb_wait_result();
}

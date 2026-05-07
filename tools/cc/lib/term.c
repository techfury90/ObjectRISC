/*
 * term.c — Object RISC libc: oriscterm interaction.
 *
 * Wraps the wire protocols documented in tools/devices/README.md
 * for the console (idx 1) and keyboard (idx 2) services. The grid,
 * vector, and pointer services aren't covered here yet — add them
 * when something needs them.
 *
 * Standard boot-ABI for terminal-using programs (caller's job to
 * arrange via --service in the right order):
 *
 *     O5  = console  (oriscterm idx 1)
 *     O6  = keyboard (oriscterm idx 2)
 *     O11 = boot stack ref  (saved by term_init)
 *     O14 = boot self-svc   (saved by term_init)
 *     O15 = boot data ref   (saved by term_init)
 *
 * term_init() saves the boot O2/O3/O4 into O11/O14/O15, attaches
 * a receive queue to the self-service, and subscribes to the
 * keyboard service. After that, every other helper here restores
 * O2/O3/O4 from the saved slots on the way out, so callers' uses
 * of print_str / print_int / hf_* etc. keep working.
 *
 * For "host stdout" printing (firmware ConsoleWrite via the legacy
 * console_io.s bridge) keep using the print_str / print_int
 * functions in io.c. term_print* writes to the Tk terminal window.
 */

#include "liborisc.h"

/* Section base VAs from CONTRACT.md §2. */
#define STACK_BOTTOM 0x001f0000
#define STACK_TOP    0x00200000
#define DATA_VA      0x00040000

/* Single-char output table — see term_print_char for why.
 *
 * SENDs to the terminal's console service are asynchronous — the
 * receiver issues the actual OBJ_READ_REQ after the CPU has moved
 * on. A naive "store c in a stack buffer, SEND offset" loses the
 * char on the next call (the stack slot gets reused before
 * fake_terminal/oriscterm reads it). The fix: precompute every
 * possible single-byte payload as the i'th byte of a static table
 * in our read-only data segment. term_print_char(c) sends offset=c
 * within that table — the data is persistent and the offset is the
 * char itself. */
const char _term_single_char_table[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
	0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
	0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
	0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
	0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
	0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
	0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
	0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
	0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
	0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
	0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
	0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

/* --- internal: restore the three OR slots libc / hf_* / hostfsd
 *     code reads from. Same shape as host_io.c::hf_restore_or. ---
 */

static void
_term_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
	asm volatile("omov o4, o14");
}

/* SEND a console-write request to the terminal's console object
 * (idx 1, in O5). source_or = "stack" picks O11; otherwise O15.
 * R4 = offset within the source object, R5 = byte count. */
static void
_term_console_write(int source_is_stack, int offset, int count)
{
	if (source_is_stack) {
		asm volatile(
			"omov  o1, o5\n"
			"omov  o2, o11\n"
			"onull o3\n"
			"addu  r4, %0, r0\n"
			"addu  r5, %1, r0\n"
			"addiu r6, r0, 0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(offset), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		asm volatile(
			"omov  o1, o5\n"
			"omov  o2, o15\n"
			"onull o3\n"
			"addu  r4, %0, r0\n"
			"addu  r5, %1, r0\n"
			"addiu r6, r0, 0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(offset), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}
	_term_restore_or();
}

/* --- term_print_only_init: park boot ORs, no keyboard subscribe -------
 *
 * For child tasks that only need to write to the Tk window (no
 * keyboard input). Skips the receive-queue attach and the
 * subscribe-to-keyboard SEND that term_init does — those would
 * compete with the parent (the shell) for keystrokes when running
 * on the same CPU. The child still inherits the parent's O5 (console
 * service ref) via TaskCreate's OPR copy, so term_print* lands on
 * the same Tk window the parent uses.
 *
 * Just three omovs: park boot O2/O3/O4 into O11/O14/O15 so
 * _term_console_write can find them via _term_restore_or. */

void
term_print_only_init(void)
{
	asm volatile("omov o11, o2");
	asm volatile("omov o14, o4");
	asm volatile("omov o15, o3");
}

/* --- term_init: park boot ORs, attach queue, subscribe ---------------- */

void
term_init(void)
{
	int status;

	/* Park the boot O2/O3/O4 into the saved slots. We use raw asm
	 * (not `register __or __asm__("o11")` style declarations) so pcc
	 * doesn't decide O11/O14/O15 are callee-save and emit a
	 * prologue that stashes the OLD value of O11 into O10 — which
	 * would smash any hostfsd / additional service ref the caller
	 * placed there at boot. */
	asm volatile("omov o11, o2");
	asm volatile("omov o14, o4");
	asm volatile("omov o15, o3");

	/* Allocate our own private mailbox service object → O9.
	 *
	 * Why not reuse the boot self-svc (O4) the way earlier versions
	 * of this code did: TaskCreate copies the parent's OPRs to the
	 * child verbatim, so a backgrounded program that calls term_init
	 * would derive its kbd subscribe-cap from the SAME O4 the
	 * parent already used — producing the same ref and getting
	 * dedup'd by oriscterm. Per-instance service objects let each
	 * program have its own kbd queue and subscribe ref, which is
	 * what the Phase 40 focus-switching depends on. */
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, 0x4103\n"     /* TAG_SERVICE */
		"addiu r6, r0, 0x5b\n"        /* R|W|S|V|C */
		"addiu r7, r0, 0\n"
		"call  #0x100\n"              /* ObjAlloc → O1 = mailbox */
		"nop\n"
		"omov  o9, o1\n"              /* O9 = term mailbox (long-lived) */
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	(void)status;

	/* Attach a receive queue (depth 16) to O9. This queue is
	 * shared: keyboard events AND hostfsd responses both land
	 * here. Known limitation — a long-running hf_read loop
	 * interleaved with keystrokes can mis-decode messages. The
	 * right fix is separate per-service queues; for now the depth
	 * is small enough that excess keystrokes during long
	 * cmd_cat / cmd_ls runs are dropped at the door rather than
	 * silently corrupting the response stream. */
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 64\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	(void)status;

	/* Derive an R|S sub-ref from O9, hand it straight to the kbd
	 * service via SEND. The derived ref lives in O2 just long
	 * enough for the SEND to copy it onto the wire; no need to
	 * park it permanently. */
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 9\n"           /* R|S */
		"call  #0x103\n"              /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"omov  o2, o1\n"              /* O2 = sub-cap for SEND */
		"omov  o1, o6\n"              /* O1 = keyboard service */
		"onull o3\n"
		"addiu r4, r0, 0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		:
		: "r1", "r4", "r5", "r6", "r7"
	);
	_term_restore_or();
}

/* --- term_shutdown: unsubscribe from the keyboard before exit --------
 *
 * oriscterm tracks each subscribed program's sub-ref in a list and
 * has no way to notice when a program TaskExits — there's no
 * process-death notification in the wire protocol. A program that
 * exits without unsubscribing leaves a dead entry in the focus-cycle
 * list, so F1 may land on it and keys silently disappear into a
 * stale queue.
 *
 * Programs that called term_init should call term_shutdown right
 * before TaskExit / main return. The protocol is "SEND R4=1,
 * O2=our sub-cap to the keyboard service" — oriscterm matches the
 * cap in its subscriber list and removes that one entry, leaving
 * other subscribers (e.g. the parent shell) untouched. The cap
 * we derive here is bit-identical to the one term_init originally
 * derived: ObjDerive on O9 (our mailbox) with caps R|S, and the
 * sim's ObjDerive is deterministic over (gen, home, idx, caps).
 *
 * Programs that crash before reaching term_shutdown will still
 * leave a dead subscriber. Robust cleanup of those is a separate
 * problem (a wire-level NACK from the home CPU when a SEND target
 * is stale would let oriscterm prune dynamically). */

void
term_shutdown(void)
{
	asm volatile(
		"omov  o1, o9\n"               /* O1 = our mailbox */
		"addiu r4, r0, 9\n"            /* R|S — same caps as term_init */
		"call  #0x103\n"               /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"omov  o2, o1\n"               /* O2 = sub-cap to unsubscribe */
		"omov  o1, o6\n"               /* O1 = keyboard service */
		"onull o3\n"
		"addiu r4, r0, 1\n"            /* R4 = 1 → unsubscribe */
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		:
		: "r1", "r4", "r5", "r6", "r7"
	);
	_term_restore_or();
}

/* --- terminal output: console-write SENDs to oriscterm ---------------- */

void
term_print(const char *s)
{
	int len = (int)strlen(s);
	unsigned int va = (unsigned int)s;
	if (va >= STACK_BOTTOM) {
		_term_console_write(1, (int)(va - STACK_BOTTOM), len);
	} else {
		_term_console_write(0, (int)(va - DATA_VA), len);
	}
}

/* Like term_print but takes an explicit byte count instead of
 * walking for a NUL — meant for binary-ish data the caller already
 * knows the length of (e.g. the result of hf_read). One SEND for the
 * whole buffer, vs one SEND per byte via term_print_char. */
void
term_print_n(const char *buf, int count)
{
	unsigned int va = (unsigned int)buf;
	if (count <= 0) return;
	if (va >= STACK_BOTTOM) {
		_term_console_write(1, (int)(va - STACK_BOTTOM), count);
	} else {
		_term_console_write(0, (int)(va - DATA_VA), count);
	}
}

/* Synchronous variant of term_print_n: SENDs to the console with
 * O3 = a reply_cap pointing at the hf mailbox (O8), then blocks
 * until the receiver acks. After the ack the bytes have been
 * pulled — the caller may now reuse the source buffer.
 *
 * This relies on hf having its own private mailbox in O8 (set up
 * by hf_init), and on the caller serializing hf_read with
 * term_print_n_sync so the mailbox queue holds at most one
 * outstanding message at any time (either an hf reply or a term
 * ack, never both). cmd_cat / cmd_ls / cmd_more in shell.c match
 * that pattern.
 *
 * Why reuse the hf mailbox instead of allocating a third one:
 * O-slot pressure. A dedicated term-sync mailbox would need its
 * own slot for the full ref, and we're already at the wall
 * (O5..O15 fully claimed). Sharing with hf is safe because of the
 * strict serialization. */
void
term_print_n_sync(const char *buf, int count)
{
	unsigned int va = (unsigned int)buf;
	int from_stack;
	int offset;
	if (count <= 0) return;
	if (va >= STACK_BOTTOM) {
		from_stack = 1;
		offset = (int)(va - STACK_BOTTOM);
	} else {
		from_stack = 0;
		offset = (int)(va - DATA_VA);
	}

	/* Derive an R|S sub-ref of the hf mailbox and stash it in O3
	 * — the SEND payload's reply_cap slot. The receiver only
	 * needs to be able to SEND back; R|S is the minimum. We park
	 * directly in O3 (vs O9 like older code did) because O9 is
	 * now the term-init kbd mailbox, which we can't clobber. */
	asm volatile(
		"omov  o1, o8\n"
		"addiu r4, r0, 9\n"             /* R|S */
		"call  #0x103\n"                /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"omov  o3, o1"                  /* O3 = reply_cap for SEND */
		:
		:
		: "r1", "r2", "r4"
	);

	if (from_stack) {
		asm volatile(
			"omov  o1, o5\n"            /* console */
			"omov  o2, o11\n"           /* stack source */
			"addu  r4, %0, r0\n"
			"addu  r5, %1, r0\n"
			"addiu r6, r0, 0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(offset), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	} else {
		asm volatile(
			"omov  o1, o5\n"
			"omov  o2, o15\n"           /* data source */
			"addu  r4, %0, r0\n"
			"addu  r5, %1, r0\n"
			"addiu r6, r0, 0\n"
			"addiu r7, r0, 0\n"
			"send  o1"
			:
			: "r"(offset), "r"(count)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}

	/* Block on the hf mailbox for the empty-payload ack. */
	asm volatile(
		"omov  o1, o8\n"
		"addiu r4, r0, -1\n"
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop"
		:
		:
		: "r1", "r2", "r3", "r4"
	);
	_term_restore_or();
}

void
term_print_char(char c)
{
	/* Use the static lookup table — see comment on
	 * _term_single_char_table for why a stack buffer doesn't work. */
	const char *src = &_term_single_char_table[(unsigned char)c];
	_term_console_write(0, (int)((unsigned int)src - DATA_VA), 1);
}

void
term_print_int(int n)
{
	/* Compute digits into a stack buf, then dispatch one character
	 * at a time via term_print_char (which uses the static table —
	 * see _term_single_char_table). Stack buffers don't work as the
	 * SEND payload because oriscterm fetches the bytes via OBJ_READ
	 * AFTER the function returns; the slots get reused first. */
	char buf[16];
	int i = 15;
	int neg = 0;

	if (n < 0) { neg = 1; n = -n; }
	if (n == 0) {
		buf[15] = '0';
		i = 14;
	} else {
		while (n > 0) {
			buf[i] = '0' + (n % 10);
			n = n / 10;
			i--;
		}
	}
	if (neg) {
		buf[i] = '-';
		i--;
	}
	for (i = i + 1; i < 16; i++) term_print_char(buf[i]);
}

void
term_print_hex(unsigned int n)
{
	int i;
	term_print_char('0');
	term_print_char('x');
	for (i = 0; i < 8; i++) {
		int nibble = (n >> ((7 - i) * 4)) & 0xF;
		term_print_char(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
	}
}

/* --- term_getkey: block for one key event from the keyboard queue ----- */

int
term_getkey(int *out_mods)
{
	int status, code, mods;
	asm volatile(
		"omov  o1, o9\n"                /* O9 = term mailbox (term_init) */
		"addiu r4, r0, -1\n"            /* infinite timeout */
		"call  #0x204\n"                /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0"
		: "=r"(status), "=r"(code), "=r"(mods)
		:
		: "r1", "r2", "r3", "r4"
	);
	_term_restore_or();
	if (status != 0) {
		if (out_mods) *out_mods = 0;
		return -1;
	}
	if (out_mods) *out_mods = mods;
	return code;
}

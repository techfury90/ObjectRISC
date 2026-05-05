/*
 * concurrent.c — N children running concurrently under cooperative
 * scheduling.
 *
 * The parent spawns five children all at once (each with a different
 * arg), then waits on each in turn. Children share a 32-byte scratch
 * object inherited via TaskCreate's OPR copy (parked in O7 by the
 * parent before any spawn) and stamp their arg into it; the parent
 * verifies each stamp landed and prints the recovered values.
 *
 * Without TaskYield in the children, the cooperative scheduler runs
 * them in spawn order: the parent's first task_wait blocks, the
 * runnable queue is [A, B, C, D, E], A runs to TaskExit, B runs, ...
 * each child stamps then exits. After E exits, the parent (waiting
 * on A) gets woken (A had already exited; TaskWait short-circuits)
 * and prints; subsequent waits short-circuit too.
 *
 * Output:
 *
 *     spawned 5 children
 *     scratch[0] = 7
 *     scratch[1] = 11
 *     scratch[2] = 13
 *     scratch[3] = 17
 *     scratch[4] = 19
 *     all done
 */

#include "liborisc.h"

#define N_KIDS 5

/* Park a derived "scratch with R+W" reference in O7 before spawning,
 * so children can OSB to it. The parent allocates it, OREFLDs the
 * ref into O1 right after ObjAlloc (which leaves it in O1), then
 * omov o7, o1.
 *
 * Children read their slot index from R4 and store R5 into the OSB. */
void
stamp_and_exit(int slot)
{
	/* R4 = slot (passed as `int n` to the entry function — pcc puts
	 * arg 1 in R4 per the calling convention). The value we want to
	 * stamp comes via inline asm reading R5, which the parent loaded
	 * with the desired byte before TaskCreate.
	 *
	 * Actually wait — TaskCreate's `init_r4` plumbing only sets R4.
	 * For a per-child custom byte we need a second channel. Easiest:
	 * encode `(slot << 8) | byte` in R4 and split here. */
	int packed = slot;
	int idx = (packed >> 8) & 0xFF;
	int val = packed & 0xFF;
	/* osb val, idx(o7) — slot index can be 0..N_KIDS-1, so a 6-byte
	 * scratch suffices. Inline asm because (a) we need to touch O7
	 * and (b) OSB takes a constant offset, but the offset here is a
	 * runtime value. Use addu to bias O7's view via a temporary
	 * register, then osb at offset 0. ... wait, OSB takes an offset,
	 * not a register-relative form. We can't add to an OR ref.
	 *
	 * Sidestep by pre-shifting the data: store at offset (idx) using
	 * a switch over idx. */
	switch (idx) {
	case 0: asm volatile("osb %0, 0(o7)" :: "r"(val)); break;
	case 1: asm volatile("osb %0, 1(o7)" :: "r"(val)); break;
	case 2: asm volatile("osb %0, 2(o7)" :: "r"(val)); break;
	case 3: asm volatile("osb %0, 3(o7)" :: "r"(val)); break;
	case 4: asm volatile("osb %0, 4(o7)" :: "r"(val)); break;
	}
	task_exit(0);
}

int
main(void)
{
	int vals[N_KIDS];
	task_t kids[N_KIDS];
	int i;

	task_init();

	vals[0] = 7;
	vals[1] = 11;
	vals[2] = 13;
	vals[3] = 17;
	vals[4] = 19;

	/* Allocate a 32-byte shared scratch object and park it in O7
	 * BEFORE any task_spawn so children inherit it via the OR copy. */
	asm volatile(
		"addiu r4, r0, 32\n"
		"addiu r5, r0, 0x4102\n"      /* TAG_DATA */
		"addiu r6, r0, 0x43\n"        /* R | W | C */
		"call  #0x100\n"              /* ObjAlloc */
		"nop\n"
		"omov  o7, o1"                /* park scratch in O7 */
		:
		:
		: "r2", "r3", "r4", "r5", "r6"
	);

	/* Spawn five children, packing (slot << 8) | val into R4. */
	for (i = 0; i < N_KIDS; i++) {
		int packed = (i << 8) | vals[i];
		kids[i] = task_spawn(stamp_and_exit, packed);
		if (kids[i] < 0) {
			print_str("task_spawn failed at slot ");
			print_int(i);
			print_str("\n");
			return 1;
		}
	}
	print_str("spawned ");
	print_int(N_KIDS);
	print_str(" children\n");

	/* Wait on each, then OLBU the corresponding scratch byte and
	 * print it. */
	for (i = 0; i < N_KIDS; i++) {
		int code = task_wait(kids[i]);
		if (code < 0) {
			print_str("task_wait failed for kid ");
			print_int(i);
			print_str("\n");
			return 1;
		}
		int byte;
		switch (i) {
		case 0: asm volatile("olbu %0, 0(o7)" : "=r"(byte)); break;
		case 1: asm volatile("olbu %0, 1(o7)" : "=r"(byte)); break;
		case 2: asm volatile("olbu %0, 2(o7)" : "=r"(byte)); break;
		case 3: asm volatile("olbu %0, 3(o7)" : "=r"(byte)); break;
		case 4: asm volatile("olbu %0, 4(o7)" : "=r"(byte)); break;
		}
		print_str("scratch[");
		print_int(i);
		print_str("] = ");
		print_int(byte);
		print_str("\n");
		task_free(kids[i]);
	}

	print_str("all done\n");
	return 0;
}

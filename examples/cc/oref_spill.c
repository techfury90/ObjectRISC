/*
 * oref_spill.c — runtime regression for `__or` autos/params as
 * per-frame OBJSTORE-memory variables (toolchain Phase 2).
 *
 * A capability passed to (or declared in) a C function and used after a
 * call cannot stay in the OR register file: every O-register is caller-
 * saved and a call clobbers them all. The backend therefore homes each
 * `__or` auto/param in a per-frame OR-typed OBJSTORE (ObjAllocStore'd in
 * the prologue, anchored in an O12 task-table slot and chained through
 * the object's slot 0 for recursion, freed in the epilogue) and reloads
 * it into an O-register transiently per use.
 *
 * This program proves the mechanism end-to-end: fwd2() receives an
 * object capability, calls sidecall() (which nulls O1..O4 to mimic a
 * clobbering call), then returns the capability. If homing works the
 * returned reference is still valid, so the marker main() wrote into the
 * object before the call reads back correctly through it.
 *
 * main() returns 42 on success (see tools/devices/tests/test_oref_spill.sh).
 */

void task_init(void);

/* A call that destroys the OR arg/return registers. */
void
sidecall(void)
{
	asm volatile("onull o1\n onull o2\n onull o3\n onull o4"
	    ::: "o1", "o2", "o3", "o4");
}

/* Hold an `__or` parameter across a clobbering call and return it.
 * `p` is homed in the per-frame OBJSTORE on entry and reloaded after
 * sidecall() for the return. */
void *__or
fwd2(void *__or p)
{
	sidecall();
	return p;
}

int
main(void)
{
	int v = 0;

	task_init();			/* sets up the O12 task table / anchor */

	asm volatile(
	    "addiu r4, r0, 64\n"	/* ObjAlloc(len=64, */
	    "addiu r5, r0, 0x4101\n"	/*          tag=TAG_DATA-ish, */
	    "addiu r6, r0, 3\n"		/*          caps=R|W) -> o1 = obj */
	    "call #0x100\n nop\n"
	    "addiu r2, r0, 2748\n"	/* 0xABC marker */
	    "osw r2, 0(o1)\n"		/* obj[0] = marker */
	    "jal fwd2\n nop\n"		/* fwd2(o1=obj) -> o1 (must == obj) */
	    "olw %0, 0(o1)\n"		/* v = returned[0] */
	    : "=r"(v)
	    :: "r2", "r3", "r4", "r5", "r6", "o1", "o2", "o3", "o4");

	return v == 2748 ? 42 : 7;
}

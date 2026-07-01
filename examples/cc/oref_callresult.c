/*
 * oref_callresult.c — runtime regression for storing a capability *call
 * result* into an `__or` home and holding it across a further call.
 *
 * This is the case the v1 backend used to REJECT ("storing an __or call
 * result into a local/param is not yet supported"): the OREFST that homes
 * a call result needs a scratch O-register, and pcc's coarse per-block
 * call liveness spuriously marked all four OR argument registers live
 * across every call, so the scratch collided with the call clobbers and
 * spilled — impossible for a capability. macdefs.h's PRUNE_CALLLIVE
 * removes that false OR liveness, so the pattern now compiles and, this
 * test proves, runs correctly.
 *
 * echo2() takes a capability, calls sidecall2() (which nulls O1..O4 to
 * mimic a clobbering call), and returns the capability. relay() then does
 * the previously-rejected thing:  void *__or x = echo2(o);  sidecall2();
 * return x;  — capturing a call RESULT into an `__or` home and holding it
 * across a further clobbering call. If homing works, the object identity
 * survives, so a marker main() wrote into the object reads back correctly.
 *
 * main() returns 42 on success (see tools/devices/tests/test_oref_spill.sh).
 */

void task_init(void);

/* A call that destroys the OR arg/return registers. */
void
sidecall2(void)
{
	asm volatile("onull o1\n onull o2\n onull o3\n onull o4"
	    ::: "o1", "o2", "o3", "o4");
}

/* Forward a capability across a clobbering call (param-across-call). */
void *__or
echo2(void *__or p)
{
	sidecall2();
	return p;
}

/*
 * The newly-enabled form: store a capability CALL RESULT into an `__or`
 * home (x), hold it across another clobbering call, then return it.
 */
void *__or
relay(void *__or o)
{
	void *__or x = echo2(o);	/* call-result -> __or home */
	sidecall2();			/* clobbers O1..O4 */
	return x;			/* reload x from its home */
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
	    "jal relay\n nop\n"		/* relay(o1=obj) -> o1 (must == obj) */
	    "olw %0, 0(o1)\n"		/* v = returned[0] */
	    : "=r"(v)
	    :: "r2", "r3", "r4", "r5", "r6", "o1", "o2", "o3", "o4");

	return v == 2748 ? 42 : 7;
}

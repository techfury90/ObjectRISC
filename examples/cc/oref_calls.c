/*
 * oref_calls.c — regression for the Object RISC `__or` calling
 * convention (toolchain Phase 1B: the OREFTY base type).
 *
 * `void *__or` is C syntax for a typed object-reference *capability*,
 * not a byte pointer. It is represented by the OREFTY base type, which
 * rides n_type and so survives tempnode / INCREF / DECREF by
 * construction — the durable "invariant by construction" the spike
 * proved (a high-bit flag in n_type, the rejected Candidate 3, was
 * shifted off the word the moment an OR sat under FTN/PTR/ARY).
 *
 * A capability can be passed, returned, stored, compared, and
 * null-tested, and it lives in the OR register file (O1..O15). It is
 * NOT pointer-arithmetic-capable (no p+n / p[i] / p++); indexed /
 * strided access is an explicit accessor operation (Phase 2 OL/OS
 * intrinsics), not C pointer math.
 *
 * This is a COMPILE-only regression (these functions never mint an OR
 * at runtime — that needs the Phase 2 intrinsics); the matching test
 * asserts the backend lowers each form to OR-file moves / O1, not the
 * GPR file or byte memory. See tools/devices/tests/test_oref_calls.sh.
 */

/* Return a capability argument unchanged: param in O1, return in O1. */
void *__or id(void *__or p) { return p; }

/* Return a null capability (0 is the only integer allowed). */
void *__or nullcap(void) { return 0; }

/* Forward a capability through another OR-returning call: arg in O1,
 * call result in O1, return in O1. */
void *__or fwd(void *__or p) { return id(p); }

/* Take several capabilities (O1..O4) plus an int (R4); return one. */
void *__or pick(void *__or a, void *__or b, void *__or c, int which)
{
	if (which == 0) return a;
	if (which == 1) return b;
	return c;
}

/* A capability stored in a local, then returned — the local must live
 * in an OR register, never byte memory. */
void *__or stash(void *__or p)
{
	void *__or local = p;
	return local;
}

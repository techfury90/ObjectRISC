/*
 * manyargs.c — regression for the Object RISC backend's 5+-argument
 * calling convention (Phase 60 / toolchain Phase 1A).
 *
 * Args 1-4 pass in R4-R7; args 5+ spill to the outgoing-arg area at
 * the bottom of the caller's frame ("sw AL, OFFSET(sp)"). Two bugs
 * used to make 5+-arg calls unusable, both fixed:
 *
 *   1. adrput() had no FUNARG case, so emitting the spill offset
 *      tripped "adrput: illegal op 57".
 *   2. funcode() discarded the outgoing-arg size, so the prologue
 *      never reserved that area — spilled args overwrote locals
 *      whenever a function both had locals and made a 5+-arg call.
 *
 * main() returns 0 on success, or a small non-zero code identifying
 * the first check that failed, so it doubles as a simorisc exit-code
 * test (see tools/devices/tests/test_manyargs.sh).
 */

int sum5(int a, int b, int c, int d, int e) { return a + b + c + d + e; }
int sum6(int a, int b, int c, int d, int e, int f) {
	return a + b + c + d + e + f;
}
int sum7(int a, int b, int c, int d, int e, int f, int g) {
	return a + b + c + d + e + f + g;
}

/* A callee with its own locals that itself makes spill-calls — the
 * combination that exposed the frame-overlap bug. */
int wrap(int a, int b, int c, int d, int e, int f, int g) {
	int lo = sum5(a, b, c, d, e);
	int hi = sum5(f, g, 1, 1, 1);
	return lo + hi - 3;            /* == a+b+c+d+e+f+g */
}

int
main(void)
{
	if (sum5(1, 2, 3, 4, 5) != 15)          return 1;
	if (sum6(1, 2, 3, 4, 5, 6) != 21)       return 2;
	if (sum7(1, 2, 3, 4, 5, 6, 7) != 28)    return 3;

	/* Multiple spill-calls + locals in one frame. */
	{
		int r5 = sum5(1, 2, 3, 4, 5);
		int r6 = sum6(1, 2, 3, 4, 5, 6);
		int r7 = sum7(1, 2, 3, 4, 5, 6, 7);
		if (r5 + r6 + r7 != 64)         return 4;
	}

	/* Nested spill-calls from a callee that has locals. */
	if (wrap(1, 2, 3, 4, 5, 6, 7) != 28)    return 5;

	return 0;
}

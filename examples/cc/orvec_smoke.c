/*
 * orvec_smoke.c — proves register-indexed OREFLD/OREFST (orefldx/orefstx)
 * end to end, via the objor_vget/objor_vset libc wrappers.
 *
 * Builds an OR-typed array of N capability slots (objor_alloc_store), stores
 * N freshly-allocated objects at RUNTIME indices, reads them back at runtime
 * indices, and verifies each round-trips as an object. Then overwrites a
 * middle slot out of order to confirm the access is true random-indexing,
 * not FIFO, and that neighbouring slots are untouched.
 *
 * Returns 42 on success; a smaller code marks the first failed step
 * (2 = array alloc, 3/6 = element alloc, 4 = null read-back, 5 = wrong
 * content, 7 = overwrite lost, 8/9 = neighbour clobbered). This is the
 * lowest-level exercise of the register-indexed addressing mode.
 */

#include "liborisc.h"
#include "obj_or.h"

#define N        6
#define BASEVAL  100

static int
run(void)
{
	void *__or vec;
	void *__or r;
	int i, v;

	/* An OR-typed array of N contiguous 8-byte OREF slots. */
	vec = objor_alloc_store(N * 8, OBJ_TAG_DATA,
	    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(vec))
		return 2;

	/* Store N distinct objects at runtime indices 0..N-1 (orefstx). */
	for (i = 0; i < N; i++) {
		r = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(r))
			return 3;
		objor_storew(r, BASEVAL + i);
		objor_vset(vec, r, i);
	}

	/* Read them back at runtime indices and verify content (orefldx). */
	for (i = 0; i < N; i++) {
		r = objor_vget(vec, i);
		if (objor_isnull(r))
			return 4;
		v = objor_loadw(r);
		if (v != BASEVAL + i)
			return 5;
	}

	/* Overwrite a middle slot out of order — random access, not FIFO. */
	r = objor_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(r))
		return 6;
	objor_storew(r, 999);
	objor_vset(vec, r, 3);
	if (objor_loadw(objor_vget(vec, 3)) != 999)
		return 7;
	if (objor_loadw(objor_vget(vec, 2)) != BASEVAL + 2)
		return 8;
	if (objor_loadw(objor_vget(vec, 4)) != BASEVAL + 4)
		return 9;

	return 42;
}

int
main(void)
{
	task_init();
	return run();
}

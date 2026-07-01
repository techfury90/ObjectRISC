/*
 * obj_or_smoke.c — end-to-end proof of the `void *__or` capability-VALUE
 * object API (obj_or.h). The value-world sibling of obj_smoke.c: same flow,
 * but capabilities are held as C `void *__or` VALUES (no handles, no table)
 * — the ergonomic API the compiler fix (PRUNE_CALLLIVE) unlocked.
 *
 * Self-contained (no services/peers): allocate an object, round-trip a
 * marker word, inspect it, derive a read-only sub-capability, read the
 * marker back THROUGH the sub (proving the sub-ref survives across the
 * intervening helper calls), drop the sub, free the object, then reuse the
 * freed descriptor slot. work() returns 42 on success; a smaller code says
 * which step failed (see tools/devices/tests/test_obj_or_smoke.sh).
 *
 * Note the structure: task_init() runs in main(), and ALL the `__or` autos
 * live in work(), a helper called AFTER task_init. This is mandatory —
 * a function that holds `__or` autos gets a per-frame OBJSTORE prologue
 * that chains through the O12 task table, which task_init() sets up; main()
 * must therefore stay free of `__or` values (its prologue runs before its
 * body can call task_init). See obj_or.h and examples/cc/oref_*.c.
 */

#include "liborisc.h"
#include "obj_or.h"

static int
work(void)
{
	void *__or o;
	void *__or sub;
	void *__or o2;

	/* Allocate a byte-addressable object. V lets us free it, C lets us
	 * derive a sub-cap, R|W lets us round-trip a marker word. */
	o = objor_alloc(64, OBJ_TAG_DATA,
	                OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_C | OBJ_CAP_V);
	if (objor_isnull(o))
		return 2;

	/* Round-trip a marker word. The core proof: `o` still names the
	 * object across the objor_storew / objor_loadw calls. */
	objor_storew(o, 2748);                /* 0xABC */
	if (objor_loadw(o) != 2748)
		return 3;

	/* Inspection. */
	if (objor_len(o) != 64)
		return 4;
	if (objor_tag(o) != OBJ_TAG_DATA)
		return 5;
	if (objor_isnull(o))
		return 6;
	if ((objor_caps(o) & OBJ_CAP_W) == 0)
		return 7;

	/* OEQ: the same capability is equal to itself. */
	if (!objor_eq(o, o))
		return 8;

	/* Derive a read-only sub-capability, held as a distinct `__or` value.
	 * This is the value-across-a-call case: `sub` is produced by a call,
	 * then used after further calls (objor_loadw, objor_eq). */
	sub = objor_derive(o, OBJ_CAP_R);
	if (objor_isnull(sub))
		return 9;
	if (objor_loadw(sub) != 2748)         /* sub-cap still sees the marker */
		return 10;
	if (!objor_eq(o, sub))                /* sub-ref names the same object */
		return 11;

	/* Drop the derived sub-ref (shares o's object — never free it), then
	 * free the object we own. */
	objor_drop(sub);
	if (objor_free(o) != 0)
		return 12;

	/* After free, the descriptor slot is reusable: a fresh alloc succeeds
	 * (typically reusing the just-freed index). */
	o2 = objor_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(o2))
		return 13;
	if (objor_free(o2) != 0)
		return 14;

	return 42;
}

int
main(void)
{
	task_init();
	return work();
}

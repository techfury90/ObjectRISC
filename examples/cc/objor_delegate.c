/*
 * objor_delegate.c — Phase-3 proof: capability DELEGATION with the
 * `void *__or` capability-value object API (obj_or.h).
 *
 * obj_or_smoke.c proves the value API inside a single function.  This proves
 * the pattern the north-stars are built on — the document architecture's
 * per-block capabilities and the object-console shell's typed-OREF pipelines
 * both hinge on it: mint a resource, derive a RESTRICTED capability, and
 * hand it to another routine AS A PLAIN C VALUE (a `__or` param) that then
 * uses it under the narrowed rights.  A capability value crossing a call
 * boundary — and a derived value being held across further calls — is
 * exactly what the v1 backend could not do before the PRUNE_CALLLIVE fix
 * (#188).  This is that capability in the shape real code will use it.
 *
 * Scenario: an owner mints a one-word "cell", grants a read-only view of it
 * to a reader, and confirms
 *   (a) the reader can READ through the grant but the grant carries no W,
 *   (b) the grant is a LIVE view — an owner write shows through it,
 *   (c) the grant names the SAME object and adds no rights over the owner.
 *
 * work() returns 42 on success; a smaller code marks the first failed check
 * (see tools/devices/tests/test_objor_delegate.sh).
 *
 * Structure rules (see obj_or.h): task_init() runs in main(); every `__or`
 * auto/param lives in a helper called after it; main() holds no `__or`
 * values (its prologue runs before its body can call task_init).
 */

#include "liborisc.h"
#include "obj_or.h"

/*
 * A delegated reader.  Receives the capability as a `__or` VALUE param — the
 * v1-unlocked case, an `__or` argument passed across a call — reads the word
 * through it, and must find the grant read-only.  Returns the word, or a
 * negative marker on a capability defect (the test's words are positive, so
 * a caller can tell a bad grant from real data).
 */
static int
read_through_grant(void *__or grant)
{
	if (objor_isnull(grant))
		return -1;
	if (objor_caps(grant) & OBJ_CAP_W)	/* granted R-only: W must be absent */
		return -2;
	return objor_loadw(grant);
}

/*
 * Confirm `grant` is a proper narrowing of `owner`.  Takes TWO `__or` params
 * in one frame (exercises the multi-`__or`-argument path): same underlying
 * object, and grant's caps a subset of owner's with W dropped.  Returns 0 if
 * consistent, else a distinct check code.
 */
static int
check_narrowing(void *__or owner_cap, void *__or grant)
{
	if (!objor_eq(owner_cap, grant))		/* same underlying object   */
		return 20;
	if (objor_caps(grant) & ~objor_caps(owner_cap))	/* adds no new rights */
		return 21;
	if (objor_caps(grant) & OBJ_CAP_W)		/* and really dropped W      */
		return 22;
	return 0;
}

static int
owner(void)
{
	int rc, w;

	/* Mint a cell we fully own: V to free, C to derive, R|W to use. */
	void *__or cell = objor_alloc(8, OBJ_TAG_DATA,
	    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_C | OBJ_CAP_V);
	if (objor_isnull(cell))
		return 2;
	objor_storew(cell, 1000);

	/* Derive a read-only grant and hand it off as a value. */
	void *__or grant = objor_derive(cell, OBJ_CAP_R);
	if (objor_isnull(grant))
		return 3;

	/* The reader reads through the grant (held across this call). */
	w = read_through_grant(grant);
	if (w != 1000)
		return (w < 0) ? (10 - w) : 4;		/* null->11, hasW->12 */

	/* The grant is genuinely a narrowing of the cell. */
	rc = check_narrowing(cell, grant);
	if (rc)
		return rc;

	/* Live view: an owner write shows through the SAME grant value. */
	objor_storew(cell, 2000);
	w = read_through_grant(grant);
	if (w != 2000)
		return (w < 0) ? (30 - w) : 5;		/* null->31, hasW->32 */

	/* Drop the sub-ref (shares the cell — never free it); free the cell. */
	objor_drop(grant);
	if (objor_free(cell) != 0)
		return 6;

	return 42;
}

int
main(void)
{
	task_init();
	return owner();
}

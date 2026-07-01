/*
 * fork_isolation.c — proves task_spawn gives a child a PRIVATE data
 * segment (fork semantics), so a child's writes to C globals do NOT
 * bleed into the parent.
 *
 * Before the fork model, task_spawn left the child mapping the parent's
 * OWN data object (TaskCreate O3 defaulted to the parent's data), so
 * parent and child SHARED all globals — a child mutating a global
 * clobbered the parent's copy, and a child's task_init wiped the
 * parent's per-task libc state. Now task_spawn hands the child a
 * byte-copy of the parent's data at spawn time.
 *
 * The test exercises both halves of fork semantics:
 *   - SNAPSHOT: the parent sets `shared = 55` BEFORE task_spawn, and the
 *     child must observe 55 (the parent's value at spawn) — not the
 *     static initializer 100, and not zero. Proves the child boots from
 *     a COPY of the parent's live data, not a fresh/zeroed segment.
 *   - ISOLATION: the child then sets `shared = 999` in its own copy; the
 *     parent must still read 55 afterward. Proves the write stayed
 *     private.
 *
 * Returns 42 on success; a smaller code names the failed check
 * (parent 2/3/9, child 20/21). No __or autos, so no per-frame OBJSTORE —
 * just the shared-global data segment the fork isolates.
 */

#include "liborisc.h"

static int shared = 100;   /* initialized global; lives in the data segment */

static int
child_work(void)
{
	if (shared != 55)          /* inherited the parent's SNAPSHOT (55), not 100/0 */
		return 20;
	shared = 999;              /* mutate MY copy */
	if (shared != 999)
		return 21;
	return 42;
}

static void
child_entry(int arg)
{
	task_init();
	task_exit(child_work());
}

static int
parent_work(void)
{
	task_t kid;
	int rc;

	shared = 55;               /* mutate BEFORE spawn — the snapshot the child sees */
	if (shared != 55)
		return 2;

	kid = task_spawn(child_entry, 0);
	if (kid < 0)
		return 3;

	rc = task_wait(kid);       /* child's checks: snapshot==55, mutate==999 */
	task_free(kid);
	if (rc != 42)
		return rc;

	if (shared != 55)          /* ISOLATION: the child's 999 must NOT have reached me */
		return 9;
	return 42;
}

int
main(void)
{
	task_init();
	return parent_work();
}

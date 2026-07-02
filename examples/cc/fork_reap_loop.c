/*
 * fork_reap_loop.c — stress the task_spawn → task_wait → task_free reap
 * path. Spawns, waits, and frees a child ITERS times in a row, reusing
 * the same table slot each iteration.
 *
 * With free-on-reap (task_free reclaims the child's stack + private data
 * copy), the object descriptors are recycled every iteration, so this
 * runs in bounded space. It is also the regression guard for the reap
 * logic itself: a bug in task_store_res / task_free_res (freeing the
 * wrong ref, a live-object double-free, or corrupting the resource
 * slots) would surface as a failed spawn/wait or a trap within a few
 * iterations. Each child mutates a global (proving the fork copy is
 * live) and returns a fixed code.
 *
 * Returns 42 on success; 2 = spawn failed, 3 = wrong exit code,
 * 4 = task_free failed, at the first bad iteration.
 */

#include "liborisc.h"

#define ITERS 24

static int counter;   /* per-task global; each child gets its own fork copy */

static int
child_work(void)
{
	counter = counter + 1;      /* mutate MY copy */
	return 20 + (counter & 1);  /* 20 or 21 — parent's copy is unchanged */
}

static void
child_entry(int arg)
{
	task_init();
	task_exit(child_work());
}

static int
run(void)
{
	int i, rc;
	task_t kid;

	counter = 100;              /* parent's copy; children never see > 101 */
	for (i = 0; i < ITERS; i++) {
		kid = task_spawn(child_entry, 0);
		if (kid < 0)
			return 2;           /* spawn failed (resource exhaustion if leaking) */
		rc = task_wait(kid);
		if (rc != 21)           /* child saw counter 100 -> 101 -> returns 21 */
			return 3;
		if (task_free(kid) != 0)
			return 4;
	}
	/* The parent's copy must be untouched by all ITERS children. */
	if (counter != 100)
		return 9;
	return 42;
}

int
main(void)
{
	task_init();
	return run();
}

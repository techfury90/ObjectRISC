/*
 * objor_console_cmd.c — the "command" half of the object-console Phase-2
 * cross-process proof. Built as /programs/command.orx and orx_spawn'd by
 * objor_console_xproc.c (the launcher).
 *
 * Unlike objor_console.c (a single program, two task_spawn'd tasks sharing
 * code), THIS is a genuinely separate program in its own address space,
 * loaded from disk. It receives its result-sink capability the way a real
 * launched command will: parked in a boot object-register (O9) by the
 * launcher and copied into this task by TaskCreate at spawn — no in-process
 * stash, no shared code. It streams N typed result objects to that sink and
 * an END marker, then exits 42.
 *
 * Returns 42; a smaller code marks the first failed check (12 = null sink,
 * 13 = alloc failed). See test_objor_console_xproc.sh.
 */

#include "liborisc.h"
#include "obj_or.h"

#define RESULT_TEXT	1
#define RESULT_END	0
#define N_RESULTS	3
#define RESULT_BASE	2001		/* content the launcher verifies */

static int
produce(void)
{
	void *__or sink;
	void *__or r;
	int i;

	sink = objor_adopt_o9();		/* the launcher's sink, inherited via O9 */
	if (objor_isnull(sink))
		return 12;
	for (i = 0; i < N_RESULTS; i++) {
		r = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(r))
			return 13;
		objor_storew(r, RESULT_BASE + i);
		objor_send_cap(sink, r, RESULT_TEXT, 0, 0, 0);
	}
	objor_send(sink, RESULT_END, 0, 0, 0);
	return 42;
}

int
main(void)
{
	task_init();
	return produce();
}

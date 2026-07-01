/* objor_console.c — object-console result-sink PoC (see test_objor_console.sh). */
#include "liborisc.h"
#include "obj_or.h"

#define RESULT_TEXT	1
#define RESULT_END	0
#define N_RESULTS	3
#define MBOX_CAPS	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

/* The command: stream N typed result objects to the sink, then close it.
 * Results aren't freed — the async SEND fetches each after we move on. */
static int
produce(void)
{
	void *__or sink;
	void *__or r;
	int i;

	sink = objor_adopt_o7();			/* the shell's result sink */
	if (objor_isnull(sink))
		return 12;
	for (i = 0; i < N_RESULTS; i++) {
		r = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(r))
			return 13;
		objor_storew(r, 1001 + i);		/* the result's content */
		objor_send_cap(sink, r, RESULT_TEXT, 0, 0, 0);
	}
	objor_send(sink, RESULT_END, 0, 0, 0);
	return 42;
}

static void
producer_entry(int arg)
{
	task_init();
	task_exit(produce());
}

/* The shell: stand up a sink, spawn the command, collect its result objects
 * in order and inspect each. */
static int
collect(void)
{
	void *__or sink;
	void *__or sink_send;
	void *__or r;
	int kind, count, got[N_RESULTS];
	int rc;
	task_t kid;

	sink = objor_alloc(16, OBJ_TAG_SERVICE, MBOX_CAPS);
	if (objor_isnull(sink))
		return 2;
	if (objor_queue_attach(sink, N_RESULTS + 1) != 0)
		return 3;
	sink_send = objor_derive(sink, OBJ_CAP_R | OBJ_CAP_S);	/* send-only for the command */
	if (objor_isnull(sink_send))
		return 4;
	objor_stash_o7(sink_send);

	kid = task_spawn(producer_entry, 0);
	if (kid < 0)
		return 5;

	count = 0;
	for (;;) {
		r = objor_recv_cap(sink, &kind);	/* blocks -> the command runs */
		if (kind == RESULT_END)
			break;
		if (kind != RESULT_TEXT)		/* the sink ABI carries the kind */
			return 6;
		if (objor_isnull(r))
			return 7;
		if (count >= N_RESULTS)
			return 8;
		got[count] = objor_loadw(r);		/* inspect the result AS AN OBJECT */
		count++;
	}

	rc = task_wait(kid);
	task_free(kid);
	if (rc != 42)
		return rc;
	if (count != N_RESULTS)
		return 30 + count;
	if (got[0] != 1001 || got[1] != 1002 || got[2] != 1003)
		return 9;
	return 42;
}

int
main(void)
{
	task_init();
	return collect();
}

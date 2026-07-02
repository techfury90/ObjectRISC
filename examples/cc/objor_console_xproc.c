/*
 * objor_console_xproc.c — object-console Phase-2 cross-process proof
 * (launcher / collector). Built as /programs/console_launcher.orx and
 * spawned BY THE SUPERVISOR (with the full spawn environment — O8=directory,
 * like sysinit gets) via a -DCONSOLE_PROOF boot hook in supervisor.c.
 *
 * It stands up a result-sink service, hands its send-cap to a SEPARATE
 * command program (/programs/command.orx, examples/cc/objor_console_cmd.c)
 * via boot register O9, orx_spawns it, collects the typed result objects it
 * streams back IN ORDER, inspects each AS AN OBJECT, verifies them, and
 * prints a PASS/FAIL marker the test greps for.
 *
 * This promotes objor_console.c's single-program PoC across a REAL process
 * boundary: the command is a distinct .orx loaded from disk, and its sink
 * capability crosses in via a boot object-register (the boot-cap result-sink
 * ABI) — copied by TaskCreate at spawn, not stashed in shared memory.
 *
 * Because orx_spawn is the supervisor's machinery, the launcher must first
 * stand up the spawn environment the way sysinit does (derive the directory
 * + hostfsd from boot O8, hf_init, orx_init, wait for the /programs mount)
 * before it can load the command. Prints "CONSOLE-PROOF PASS" on success;
 * "CONSOLE-PROOF FAIL <code>" otherwise (10 env, 2..9/30+ collect,
 * 21..26 = orx_spawn errno, 5 = command exited non-42).
 */

#include "liborisc.h"
#include "obj_or.h"

#define RESULT_TEXT	1
#define RESULT_END	0
#define N_RESULTS	3
#define RESULT_BASE	2001
#define MBOX_CAPS	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

/* Stand up the orx_spawn environment (mirrors sysinit.c): boot O8 is the
 * directory — publish it to DIR_SLOT (O12 584) + ORX_SLOT_CHILD_O8 (560),
 * derive hostfsd into O10 (616 = DIR_RESULT_SLOT), hf_init + orx_init, then
 * wait for hostfsd to register the /programs MOUNT. Returns 0, or 10 on
 * hf_init failure. No __or autos here (pure asm + int helpers). */
static int
setup_env(void)
{
	int kind, attempt;
	char rem[16];

	asm volatile(
		"orefld o1, 544(o12)\n"    /* BOOT_PARENT_SLOT = boot O8 = directory */
		"orefst o1, 584(o12)\n"    /* -> DIR_SLOT (our dir_walk resolution) */
		"orefst o1, 560(o12)"      /* -> ORX_SLOT_CHILD_O8 (unused here) */
		: : : "r1");
	if (dir_walk("/sys/hostfsd/0", &kind, rem, sizeof(rem)) >= 0)
		asm volatile("orefld o10, 616(o12)");   /* DIR_RESULT_SLOT -> O10 */
	if (hf_init() != 0)
		return 10;
	orx_init();
	for (attempt = 0; attempt < 400; attempt++) {
		if (dir_walk("/programs", &kind, rem, sizeof(rem)) >= 0
		    && kind == DIR_KIND_MOUNT)
			break;
		task_yield();
	}
	return 0;
}

static int
collect(void)
{
	void *__or sink;
	void *__or sink_send;
	void *__or r;
	int kind, count, got[N_RESULTS];
	int rc, i;
	task_t kid;

	sink = objor_alloc(16, OBJ_TAG_SERVICE, MBOX_CAPS);
	if (objor_isnull(sink))
		return 2;
	if (objor_queue_attach(sink, N_RESULTS + 1) != 0)
		return 3;
	sink_send = objor_derive(sink, OBJ_CAP_R | OBJ_CAP_S);	/* send-only for the command */
	if (objor_isnull(sink_send))
		return 4;
	objor_stash_o9(sink_send);		/* hand the command its sink via O9 */

	kid = orx_spawn("/programs/command.orx", "", "/");
	if (kid < 0)
		return 20 - kid;		/* 21..26 = -orx_spawn errno */

	count = 0;
	for (;;) {
		r = objor_recv_cap(sink, &kind);	/* blocks -> the command runs */
		if (kind == RESULT_END)
			break;
		if (kind != RESULT_TEXT)
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
		return 5;			/* command didn't exit cleanly */
	if (count != N_RESULTS)
		return 30 + count;
	for (i = 0; i < N_RESULTS; i++)
		if (got[i] != RESULT_BASE + i)
			return 9;
	return 42;
}

int
main(void)
{
	int rc;

	task_init();
	rc = setup_env();
	if (rc == 0)
		rc = collect();
	if (rc == 42)
		print_str("CONSOLE-PROOF PASS: 3 typed results collected from command.orx\n");
	else {
		print_str("CONSOLE-PROOF FAIL ");
		print_int(rc);
		print_char('\n');
	}
	return rc;
}

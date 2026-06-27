/*
 * sysinit.c — the terminal launcher (M3).
 *
 * Repositioned from a one-shot "online" stub into THE boot-time launcher.  The
 * leader supervisor spawns sysinit as its only leader child; sysinit brings up
 * the window manager (and, later, optional services) as same-CPU tasks, then
 * stays alive (idle-yielding) owning them.  The login chain is gone — the WM
 * boots to an empty Workspace and apps launch from its right-click menu
 * (oriscwm desktop_menu_select -> sup_spawn).
 *
 * Boot ABI (forwarded by the supervisor at spawn):
 *   O5  = the firmware's display-backed framebuffer, relayed down the chain
 *         termfw -> supervisor -> here.  We forward it on to the WM, which
 *         ADOPTS it instead of allocating a 2nd display = ONE seamless window.
 *   O8  = the directory (NOT the spawn-mailbox sub-cap sysinit used to get):
 *         we orx_spawn the WM ourselves, and the WM needs O8 = directory for
 *         its self_register / dir_register.
 *   O10 = hostfsd (also re-derived from the directory below).
 */

#include "liborisc.h"

#define WM_PATH "/programs/oriscwm.orx"

static void restore_or_state(void) { asm volatile("omov o2, o11\nomov o3, o15"); }
#define SP(s) do { restore_or_state(); print_str(s); } while (0)
#define SI(n) do { restore_or_state(); print_int(n); } while (0)

int
main(void)
{
	int attempt;

	task_init();

	/* Stash the inherited framebuffer (O5) into ORX_SLOT_CHILD_O5 NOW, before
	 * any later step could clobber the passthrough OPR — we relay it to the WM.
	 * If no FB was forwarded (O5 null), this forwards null and the WM falls back
	 * to allocating its own (the separate-CPU path). */
	asm volatile("orefst o5, 632(o12)");   /* ORX_SLOT_CHILD_O5 = framebuffer */

	SP("sysinit: launcher starting\n");

	/* Directory (boot O8) -> DIR_SLOT (our orx_spawn path resolution) AND ->
	 * ORX_SLOT_CHILD_O8, so the WM harvests O8 = directory for self_register. */
	asm volatile(
		"orefld o1, 544(o12)\n"
		"orefst o1, 584(o12)\n"   /* DIR_SLOT */
		"orefst o1, 560(o12)"     /* ORX_SLOT_CHILD_O8 = directory */
		: : : "r1");

	/* Derive hostfsd + orx state from the directory so we can orx_spawn. */
	{
		int kind;
		char rem[16];
		if (dir_walk("/sys/hostfsd/0", &kind, rem, sizeof(rem)) >= 0)
			asm volatile("orefld o10, 616(o12)");   /* DIR_RESULT_SLOT -> O10 */
	}
	if (hf_init() != 0) { SP("sysinit: FAIL hf_init\n"); return 1; }
	orx_init();

	/* Wait for the /programs mount, then launch the window manager. */
	{
		int kind = 0;
		char rem[16];
		for (attempt = 0; attempt < 400; attempt++) {
			if (dir_walk("/programs", &kind, rem, sizeof(rem)) >= 0
			    && kind == DIR_KIND_MOUNT)
				break;
			task_yield();
		}
	}
	{
		task_t wm = orx_spawn(WM_PATH, "", "/");
		if (wm < 0) { SP("sysinit: FAIL launch WM rc="); SI((int)wm); SP("\n"); return 1; }
	}
	SP("sysinit: window manager launched\n");

	/* (Future) optional services launch here, the same way. */

	/* Stay alive owning the WM (+ services).  A launcher that exited would let
	 * the supervisor's task_resume hit an already-exited task (the M2 wrinkle);
	 * idle-yielding keeps us schedulable instead. */
	for (;;)
		task_yield();
	return 0;
}

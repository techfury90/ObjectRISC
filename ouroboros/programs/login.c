/*
 * login.c — Phase 48: per-terminal session manager.
 *
 * Spawned by every terminal-equipped supervisor in place of
 * /programs/shell.orx. Loops:
 *
 *   1. Wipe both panes (text via term_clear / form-feed; canvas via
 *      grid_clear).
 *   2. Print the welcome banner.
 *   3. Block on the keyboard until the user presses Enter.
 *   4. sup_spawn /programs/shell.orx as a child; task_wait for it.
 *   5. When the shell exits, orx_unload it and loop back to (1).
 *
 * Two paths out of the shell, distinguished by what the shell does:
 *
 *   - `logout`     → shell prints "logged out\n" and `return 0;`
 *                    from main. task_wait returns 0; login wipes
 *                    the screen and loops back to the welcome
 *                    banner. The supervisor stays alive — only
 *                    THIS session ended.
 *   - `exit`/`quit`→ shell calls sup_shutdown() (op=2 to local
 *                    supervisor), then exits. The supervisor halts
 *                    on op=2; killing its dispatch loop also kills
 *                    every task it owns, including login. login
 *                    never sees task_wait return — it's already
 *                    dead. oriscrun's --leader-timeout watches the
 *                    leader CPU and tears the system down.
 *
 * Boot ABI inherited from the supervisor:
 *   O5/O6/O7 = terminal console/keyboard/grid (per-CPU after Phase 47)
 *   O8       = supervisor spawn-mailbox sub-cap (via ORX_SLOT_CHILD_O8)
 *   O10      = hostfsd
 *
 * task_init harvests O8 → BOOT_PARENT_SLOT, which sup_spawn reads to
 * find our supervisor. term_init subscribes our private mailbox to
 * the keyboard service. From there it's all standard libc.
 */

#include "liborisc.h"

#define SHELL_PATH "/programs/shell.orx"

const char banner_pre[]  =
	"\n"
	"  Welcome to the Ouroboros operating system for Object RISC.\n"
	"\n"
	"  Press Enter to begin a session.\n";

const char spawn_failed[] = "login: failed to spawn shell: ";
const char spawn_retry[]  = " (retrying)\n";
const char nl[]           = "\n";

int
main(void)
{
	task_init();
	term_init();        /* subscribes our private mailbox to the
	                     * terminal's keyboard service; subsequent
	                     * term_getkey calls block on it. */
	hf_init();          /* not strictly required by login, but the
	                     * spawned shell needs it set up — and orx's
	                     * loader uses hf for opening shell.orx. */

	for (;;) {
		/* Wipe both panes. Phase 48: term_clear sends 0x0C which
		 * oriscterm + fake_terminal both interpret as text-pane
		 * clear; grid_clear blanks the canvas. */
		term_clear();
		grid_clear();

		term_print(banner_pre);

		/* Wait for Enter. Discard everything else (so e.g. an
		 * accidental keystroke at the welcome screen doesn't
		 * carry through to the shell). */
		for (;;) {
			int mods;
			int c = term_getkey(&mods);
			if (c == TK_RETURN) break;
		}

		/* Unsubscribe from the keyboard before spawning the shell.
		 * Otherwise BOTH login AND shell would be in the keyboard
		 * subscriber list with login's focus_idx=0 keeping focus —
		 * shell would never receive keystrokes and the user would
		 * be stuck staring at a non-responsive prompt. The shell's
		 * own term_init (via task_init's OPR inheritance) re-
		 * subscribes when it starts; while it's the sole subscriber
		 * (focus_idx=0 again), it gets all keys. When shell exits
		 * we re-call term_init below to reattach login's
		 * subscription for the next welcome cycle. */
		term_shutdown();

		/* Spawn the shell via the supervisor (sup_spawn → SEND op=1).
		 * Same path the user-visible `run` command uses — keeps the
		 * supervisor as the sole spawn-server, so the shell's own
		 * subsequent `run` requests ride the same path. cwd "/" is
		 * the conventional starting directory. */
		task_t shell = sup_spawn(SHELL_PATH, "", "/");
		if (shell < 0) {
			term_print(spawn_failed);
			term_print_int((int)shell);
			term_print(spawn_retry);
			/* Re-attach our keyboard subscription before looping;
			 * the next iteration's welcome banner needs it.
			 * term_resubscribe (not term_init) — see comment
			 * at the resubscribe call below. */
			term_resubscribe();
			task_yield();
			continue;
		}

		/* task_wait blocks until the shell exits. orx_unload runs
		 * task_wait + manifest cleanup + task_free; we don't have a
		 * manifest for sup_spawn'd children (the supervisor owns
		 * that), but orx_unload swallows EFAULT on the manifest
		 * frees, so it's safe — and it gives us the standard
		 * "release the local handle" path. */
		(void)orx_unload(shell);

		/* Re-attach to keyboard for the next welcome cycle. We
		 * use term_resubscribe (NOT term_init) because term_init
		 * would re-save O2/O3/O4 into O11/O14/O15 — but by now
		 * those OPRs have been clobbered by sup_spawn / orx_unload
		 * etc., so the resave would corrupt the boot-OR snapshot
		 * that subsequent term_print calls depend on for
		 * data-segment string sources. term_resubscribe just
		 * derives a fresh sub-cap from our existing mailbox in O9
		 * and SENDs the subscribe op. */
		term_resubscribe();

		/* Loop back to welcome banner. Whether the shell ran
		 * `logout` (clean exit, code 0) or any other clean exit,
		 * login starts a fresh session. `exit`/`quit` would have
		 * killed us via sup_shutdown so we never got here in
		 * that case. */
		term_print(nl);
	}
}

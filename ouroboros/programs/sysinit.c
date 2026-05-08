/*
 * sysinit.c — Phase 48: leader-only system-init task.
 *
 * The leader supervisor spawns this as a one-shot task right after
 * mounting /programs and right before spawning login.orx on each
 * terminal-equipped CPU. It's the conventional Unix-style "init"
 * slot — the place to do system-wide setup that only one CPU
 * should perform.
 *
 * Currently sysinit has nothing required to do. The /programs MOUNT
 * was originally going to live here, but supervisor needs that
 * mount to be in place BEFORE it can orx_spawn anything from
 * /programs/ (sysinit included). We tried; the bootstrap chicken-
 * and-egg made everything unable to spawn. So /programs stays
 * inline in supervisor.c, and sysinit is left as a placeholder
 * for future late-boot work — populating /sys/version, registering
 * a system-wide configuration service, kicking off log rotation,
 * etc.
 *
 * For now: print a one-line "online" banner to firmware stdout
 * (the boot console, not the Tk terminal — login.orx is about to
 * paint over the Tk window anyway), then exit clean.
 *
 * Boot ABI inherited from supervisor at spawn:
 *   O5/O6/O7 = oriscterm console/keyboard/grid (unused here)
 *   O8       = supervisor's spawn-mailbox sub-cap (via
 *              ORX_SLOT_CHILD_O8; task_init parks it in
 *              BOOT_PARENT_SLOT — sup_spawn would work if we
 *              ever wanted it)
 *   O10      = hostfsd
 */

#include "liborisc.h"

int
main(void)
{
	task_init();

	print_str("sysinit: online\n");

	/* Future system-wide setup work goes here. */

	return 0;
}

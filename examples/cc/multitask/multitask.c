/*
 * multitask.c — Object RISC tasks API demo.
 *
 * Spawns three child tasks in sequence — each takes its R4 arg,
 * doubles it, and exits with that value as its exit code. Parent
 * waits, prints the result, frees the child, repeats.
 *
 * Demonstrates:
 *   - task_init parking the boot code ref so subsequent task_spawn
 *     calls can hand it to TaskCreate
 *   - task_spawn(fn, arg) creating + resuming a child
 *   - task_wait surfacing the child's exit code via R3
 *   - task_free reaping the descriptor (allowing task_spawn to
 *     reuse the slot)
 *
 * Output (via host stdout / firmware ConsoleWrite — the legacy
 * print_str path; no terminal/oriscterm needed):
 *
 *     child(7) -> 14
 *     child(11) -> 22
 *     child(21) -> 42
 *     parent done
 *
 * Build via examples/cc/multitask/run.sh.
 */

#include "liborisc.h"

void
double_and_exit(int n)
{
	task_exit(n * 2);
}

int
main(void)
{
	int args[3];
	int i;
	int result;

	/* MUST be called before main clobbers O1. crt0 calls main with
	 * O1 still holding the boot code ref; task_init parks it in O13. */
	task_init();

	args[0] = 7;
	args[1] = 11;
	args[2] = 21;

	for (i = 0; i < 3; i++) {
		task_t kid = task_spawn(double_and_exit, args[i]);
		if (kid < 0) {
			print_str("task_spawn failed\n");
			return 1;
		}
		result = task_wait(kid);
		if (result < 0) {
			print_str("task_wait failed\n");
			return 1;
		}
		print_str("child(");
		print_int(args[i]);
		print_str(") -> ");
		print_int(result);
		print_str("\n");
		task_free(kid);
	}

	print_str("parent done\n");
	return 0;
}

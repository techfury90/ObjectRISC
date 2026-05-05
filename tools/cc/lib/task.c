/*
 * task.c — Object RISC libc: task management.
 *
 * Wraps Vol VI §4 task primitives plus the matching ObjFree-of-task
 * reaping path. The MVP is single-child: each task_* call operates
 * on the task ref currently parked in O12. Multi-child programs
 * need to omov refs between O12 and other slots themselves until
 * the libc grows a real task table.
 *
 * Boot-ABI required of callers
 * ----------------------------
 * task_init() must be called once at program start, before main
 * clobbers O1 (the boot code ref). It parks the boot O1/O2/O3 in
 * O13/O11/O15 so subsequent task_spawn calls can hand the code ref
 * to TaskCreate and restore O2/O3 after the call (console_write,
 * print_str, etc. read string data through O2/O3).
 *
 *     O11 = boot stack ref               (parked by task_init)
 *     O12 = current child task ref       (set by task_spawn)
 *     O13 = parent's boot code ref       (parked by task_init)
 *     O15 = boot data ref                (parked by task_init)
 *
 * Slot choices match the term.c boot-save convention (O11 = stack,
 * O15 = data) so a program using both task.c and term.c gets one
 * coherent set of boot saves regardless of init order. O13 is
 * task-specific (term.c doesn't use it). O14 (term's self-svc save)
 * is left untouched.
 *
 * Children inherit the parent's OPRs verbatim (TaskCreate copies
 * O1..O15, with O0 forced null) — so they see the same O5..O10
 * service refs the parent had at task_spawn time, the same O11/O15
 * boot saves, etc. That's how a child can call print_str /
 * hf_open / term_print without redoing the init dances.
 */

#include "liborisc.h"

#define CODE_VA 0x00010000

#define TAG_STACK    0x4101
#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_C 0x40
#define DEFAULT_STACK_SIZE  0x1000   /* 4 KiB — fine for leaf children */

/* --- task_init: park boot O1/O2/O3 into O13/O11/O15 ------------- */

void
task_init(void)
{
	asm volatile(
		"omov o13, o1\n"     /* boot code ref */
		"omov o11, o2\n"     /* boot stack ref */
		"omov o15, o3"       /* boot data ref */
	);
}

/* --- task_yield: surrender the rest of the quantum -------------- */

void
task_yield(void)
{
	asm volatile(
		"call #0x004\n"
		"nop"
		:
		:
		: "r2"
	);
}

/* --- task_exit: terminate the calling task (does not return) ---- */

void
task_exit(int code)
{
	asm volatile(
		"addu r4, %0, r0\n"
		"call #0x001\n"
		"nop"
		:
		: "r"(code)
		: "r4"
	);
}

/* --- task_spawn: create + resume a child --------------------------
 *
 * Allocates a fresh stack via ObjAlloc, calls TaskCreate with the
 * parent's code ref (parked in O13 by task_init) and the supplied
 * entry offset, then TaskResume. The new task's ref is left in O12
 * for subsequent task_wait / task_free.
 *
 * `entry` is a function pointer; we convert it to the byte offset
 * within the code object by subtracting CODE_VA (the standard
 * mapping VA per CONTRACT.md §2). `arg` is placed in the child's
 * R4 so the entry function can read it as its first integer arg.
 *
 * Returns 0 on success, or the error code from a failed primitive.
 */
int
task_spawn(void (*entry)(int), int arg)
{
	unsigned int entry_off = (unsigned int)entry - CODE_VA;
	int status;

	/* ObjAlloc(R4=size, R5=tag, R6=caps) → O1 = stack ref. */
	asm volatile(
		"addiu r4, r0, %1\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x100\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(DEFAULT_STACK_SIZE), "i"(TAG_STACK), "i"(CAP_R | CAP_W | CAP_C)
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status != 0)
		return status;

	/* TaskCreate(O1=code, O2=stack, R4=entry_off, R5=arg) → O1 = task.
	 * Restore O2/O3 from O11/O15 afterwards — TaskCreate clobbered O2
	 * (we set it to the child's stack) and console_write / print_str
	 * use O2 (stack data) and O3 (segment data) to find string memory. */
	asm volatile(
		"omov  o2, o1\n"           /* O2 = stack ref (just from ObjAlloc) */
		"omov  o1, o13\n"          /* O1 = parent's code ref */
		"addu  r4, %1, r0\n"
		"addu  r5, %2, r0\n"
		"call  #0x000\n"
		"nop\n"
		"omov  o12, o1\n"          /* park child task ref in O12 */
		"omov  o2, o11\n"          /* restore parent's stack ref */
		"omov  o3, o15\n"          /* restore parent's data ref */
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(entry_off), "r"(arg)
		: "r2", "r3", "r4", "r5"
	);
	if (status != 0)
		return status;

	/* TaskResume(O1=task). */
	asm volatile(
		"omov  o1, o12\n"
		"call  #0x002\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2"
	);
	return status;
}

/* --- task_wait: block until the child exits, return its code ----
 *
 * The child task is taken from O12 (where task_spawn parked it).
 * On wakeup, the firmware places R3 = exit code; we surface that as
 * the return value. Errors return negative (R2 negated to keep the
 * happy-path 0..255 range usable).
 */
int
task_wait(void)
{
	int code;
	int status;

	asm volatile(
		"omov  o1, o12\n"
		"call  #0x007\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(status), "=r"(code)
		:
		: "r2", "r3"
	);
	if (status != 0)
		return -status;
	return code;
}

/* --- task_free: reap the child's descriptor --------------------- */

int
task_free(void)
{
	int status;
	asm volatile(
		"omov  o1, o12\n"
		"call  #0x101\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2"
	);
	return status;
}

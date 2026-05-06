/*
 * task.c — Object RISC libc: task management.
 *
 * Wraps Vol VI §4 task primitives plus the matching ObjFree-of-task
 * reaping path. Multi-child API: each call takes a `task_t` handle
 * that names a slot in a libc-managed OREF storage table holding
 * up to TASK_MAX_CONCURRENT child task refs at once.
 *
 *     task_t kid_a = task_spawn(child_a, 7);
 *     task_t kid_b = task_spawn(child_b, 11);
 *     int exit_a = task_wait(kid_a);
 *     int exit_b = task_wait(kid_b);
 *     task_free(kid_a);
 *     task_free(kid_b);
 *
 * The table itself is an OR-typed storage object (ObjAllocStore'd
 * by task_init), parked in O12 with R+W caps. Slot index → byte
 * offset is `slot * 8` (each OR ref is 64 bits). A separate in-use
 * bitmap (regular int memory) tracks which slots hold live refs so
 * task_spawn can find a free slot in O(MAX) without scanning the
 * OREF table itself.
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
 *     O12 = task table (objstore ref)    (allocated by task_init)
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

#define CODE_VA            0x00010000
#define TAG_DATA           0x4102
#define TAG_STACK          0x4101
#define CAP_R              0x01
#define CAP_W              0x02
#define CAP_C              0x40
#define DEFAULT_STACK_SIZE 0x1000   /* 4 KiB — fine for leaf children */

/* TASK_MAX_CONCURRENT is defined in liborisc.h so callers can size
 * arrays of task_t against it. Keep them in sync. */
#define TABLE_BYTES (TASK_MAX_CONCURRENT * 8)

/* The task-table objstore doubles as orx.c's persistent state. orx
 * picks up at byte offset TABLE_BYTES — see tools/cc/lib/orx.c for
 * the layout there. We oversize the allocation here so orx doesn't
 * have to re-allocate (and doesn't have to claim its own OPR slot —
 * that frees O7 for the grid service ref). */
#define ORX_STATE_BYTES   408
#define ALLOC_BYTES       (TABLE_BYTES + ORX_STATE_BYTES)

/* Bit set when the corresponding table slot holds a live ref. Lives
 * in regular int memory; pcc treats it as a normal global. */
static unsigned int task_slots_in_use;

/* --- task_init: park boot ORs + ObjAllocStore the table --------- */

void
task_init(void)
{
	asm volatile(
		"omov o13, o1\n"     /* boot code ref */
		"omov o11, o2\n"     /* boot stack ref */
		"omov o15, o3\n"     /* boot data ref */

		/* ObjAllocStore(R4=size, R5=tag, R6=caps) → O1 = table ref. */
		"addiu r4, r0, %0\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x106\n"
		"nop\n"
		"omov  o12, o1"
		:
		: "i"(ALLOC_BYTES), "i"(TAG_DATA), "i"(CAP_R | CAP_W)
		: "r2", "r3", "r4", "r5", "r6"
	);
	task_slots_in_use = 0;
}

/* --- internal: OREFLD slot → O1 / OREFST O1 → slot ---------------
 *
 * OREFLD/OREFST take a constant 16-bit signed offset; we can't
 * compute the slot offset at runtime in a single instruction. The
 * switches below are mechanical — pcc lowers them to a chain of
 * compare-branches, which is fine at TASK_MAX_CONCURRENT = 16. */

static void
task_load_to_o1(int slot)
{
	switch (slot) {
	case  0: asm volatile("orefld o1, 0(o12)");   break;
	case  1: asm volatile("orefld o1, 8(o12)");   break;
	case  2: asm volatile("orefld o1, 16(o12)");  break;
	case  3: asm volatile("orefld o1, 24(o12)");  break;
	case  4: asm volatile("orefld o1, 32(o12)");  break;
	case  5: asm volatile("orefld o1, 40(o12)");  break;
	case  6: asm volatile("orefld o1, 48(o12)");  break;
	case  7: asm volatile("orefld o1, 56(o12)");  break;
	case  8: asm volatile("orefld o1, 64(o12)");  break;
	case  9: asm volatile("orefld o1, 72(o12)");  break;
	case 10: asm volatile("orefld o1, 80(o12)");  break;
	case 11: asm volatile("orefld o1, 88(o12)");  break;
	case 12: asm volatile("orefld o1, 96(o12)");  break;
	case 13: asm volatile("orefld o1, 104(o12)"); break;
	case 14: asm volatile("orefld o1, 112(o12)"); break;
	case 15: asm volatile("orefld o1, 120(o12)"); break;
	}
}

static void
task_store_from_o1(int slot)
{
	switch (slot) {
	case  0: asm volatile("orefst o1, 0(o12)");   break;
	case  1: asm volatile("orefst o1, 8(o12)");   break;
	case  2: asm volatile("orefst o1, 16(o12)");  break;
	case  3: asm volatile("orefst o1, 24(o12)");  break;
	case  4: asm volatile("orefst o1, 32(o12)");  break;
	case  5: asm volatile("orefst o1, 40(o12)");  break;
	case  6: asm volatile("orefst o1, 48(o12)");  break;
	case  7: asm volatile("orefst o1, 56(o12)");  break;
	case  8: asm volatile("orefst o1, 64(o12)");  break;
	case  9: asm volatile("orefst o1, 72(o12)");  break;
	case 10: asm volatile("orefst o1, 80(o12)");  break;
	case 11: asm volatile("orefst o1, 88(o12)");  break;
	case 12: asm volatile("orefst o1, 96(o12)");  break;
	case 13: asm volatile("orefst o1, 104(o12)"); break;
	case 14: asm volatile("orefst o1, 112(o12)"); break;
	case 15: asm volatile("orefst o1, 120(o12)"); break;
	}
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
 * entry offset, OREFSTs the new ref into the next free table slot,
 * then TaskResumes. Restores O2/O3 from O11/O15 on the way out so
 * the caller's subsequent print_str / print_int keep working.
 *
 * Returns the slot index (>= 0) on success; -firmware_errno on a
 * primitive failure; -1 if the table is full.
 */
task_t
task_spawn(void (*entry)(int), int arg)
{
	unsigned int entry_off = (unsigned int)entry - CODE_VA;
	int slot;
	int status;

	/* Find a free slot. */
	for (slot = 0; slot < TASK_MAX_CONCURRENT; slot++) {
		if (!(task_slots_in_use & (1 << slot)))
			break;
	}
	if (slot >= TASK_MAX_CONCURRENT)
		return -1;

	/* ObjAlloc(R4=size, R5=TAG_STACK, R6=R|W|C) → O1 = stack ref. */
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
		return -status;

	/* TaskCreate(O1=code, O2=stack, R4=entry_off, R5=arg) → O1 = task. */
	asm volatile(
		"omov  o2, o1\n"           /* O2 = stack ref (just from ObjAlloc) */
		"omov  o1, o13\n"          /* O1 = parent's code ref */
		"addu  r4, %1, r0\n"
		"addu  r5, %2, r0\n"
		"call  #0x000\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(entry_off), "r"(arg)
		: "r2", "r3", "r4", "r5"
	);
	if (status != 0) {
		/* Restore O2/O3 before bailing — caller's prints depend on them. */
		asm volatile("omov o2, o11");
		asm volatile("omov o3, o15");
		return -status;
	}

	/* O1 now holds the new task ref. Park it in the table at `slot`. */
	task_store_from_o1(slot);
	task_slots_in_use |= (1 << slot);

	/* TaskResume(O1=task) — O1 still holds the ref from TaskCreate. */
	asm volatile(
		"call  #0x002\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2"
	);

	/* Restore O2/O3 for the caller. */
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");

	if (status != 0)
		return -status;
	return slot;
}

/* --- task_register_o1 / task_resume — handles for tasks the libc
 * didn't create itself.
 *
 * orx.c's loader calls TaskCreate directly (it has its own
 * code/data/stack-allocation flow), then needs to enroll the
 * resulting task into the libc table so the user can task_wait /
 * task_free it via the normal handle API. task_register_o1 finds
 * a free slot, OREFSTs O1 (assumed to hold the task ref) into it,
 * and returns the handle. task_resume then drives the named task
 * to RUNNABLE — the equivalent of task_spawn's TaskResume step,
 * but separated out so loaders can interleave their own work
 * between TaskCreate and TaskResume.
 *
 * Returns -1 if the table is full (task_register_o1) or the
 * handle is invalid (task_resume); otherwise the firmware status
 * is propagated negated. */

task_t
task_register_o1(void)
{
	int slot;
	for (slot = 0; slot < TASK_MAX_CONCURRENT; slot++) {
		if (!(task_slots_in_use & (1 << slot)))
			break;
	}
	if (slot >= TASK_MAX_CONCURRENT)
		return -1;
	task_store_from_o1(slot);
	task_slots_in_use |= (1 << slot);
	return slot;
}

int
task_resume(task_t t)
{
	int status;
	if (t < 0 || t >= TASK_MAX_CONCURRENT
			|| !(task_slots_in_use & (1 << t)))
		return -1;
	task_load_to_o1(t);
	asm volatile(
		"call  #0x002\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2"
	);
	if (status != 0)
		return -status;
	return 0;
}

/* --- task_kill: externally terminate a task ----------------------
 *
 * Loads the child's ref from the table into O1, sets R4 = exit
 * code, calls TaskKill (#0x00A). The descriptor stays parked in
 * its slot — the caller still needs task_wait + task_free (or
 * orx_unload) to actually reclaim it. Idempotent: killing a task
 * that has already exited returns 0.
 */
int
task_kill(task_t t, int code)
{
	int status;

	if (t < 0 || t >= TASK_MAX_CONCURRENT
			|| !(task_slots_in_use & (1 << t)))
		return -1;

	task_load_to_o1(t);
	asm volatile(
		"addu  r4, %1, r0\n"
		"call  #0x00A\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(code)
		: "r2", "r4"
	);
	if (status != 0)
		return -status;
	return 0;
}

/* --- task_wait: block until the named child exits, return its code
 *
 * Loads the child's ref from the table into O1, calls TaskWait. On
 * wakeup the firmware places R3 = exit code; we surface that as the
 * return value. Errors return negative (firmware error code negated
 * to keep the happy-path 0..255 range usable).
 */
int
task_wait(task_t t)
{
	int status, code;

	if (t < 0 || t >= TASK_MAX_CONCURRENT
			|| !(task_slots_in_use & (1 << t)))
		return -1;

	task_load_to_o1(t);
	asm volatile(
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

/* --- task_free: reap the child's descriptor and free its slot --- */

int
task_free(task_t t)
{
	int status;

	if (t < 0 || t >= TASK_MAX_CONCURRENT
			|| !(task_slots_in_use & (1 << t)))
		return -1;

	task_load_to_o1(t);
	asm volatile(
		"call  #0x101\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2"
	);
	if (status == 0) {
		/* Clear the slot — overwrite with the null ref so any stale
		 * reuse via task_load_to_o1 reads back as null, and free up
		 * the bit for the next task_spawn. */
		asm volatile("omov o1, o0");
		task_store_from_o1(t);
		task_slots_in_use &= ~(1 << t);
	}
	return status;
}

/* --- task_query / task_active_mask — non-blocking inspection ----
 *
 * Vol VI #0x008 returns a packed state word: state in low 8 bits,
 * processor id in next 8, exit code in upper 16 (only meaningful
 * when state == TASK_STATE_EXITED). We unpack into the
 * caller-supplied task_info_t for ergonomics.
 *
 * task_active_mask returns the libc's internal bitmap of in-use
 * task table slots so the shell can iterate `jobs` and the
 * auto-reaper can poll without taking locks. */

int
task_query(task_t t, struct task_info *out)
{
	int status, packed;

	if (t < 0 || t >= TASK_MAX_CONCURRENT
			|| !(task_slots_in_use & (1 << t)))
		return -1;
	task_load_to_o1(t);
	asm volatile(
		"call  #0x008\n"
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(status), "=r"(packed)
		:
		: "r2", "r3"
	);
	if (status != 0)
		return -status;
	out->state     = packed & 0xff;
	out->processor = (packed >> 8) & 0xff;
	out->exit_code = (packed >> 16) & 0xff;
	return 0;
}

unsigned int
task_active_mask(void)
{
	return task_slots_in_use;
}

/* --- task_install_preempt_timer — wire the timer handler --------
 *
 * Installs preempt_handler.s::preempt_timer_handler as the
 * supervisor handler for cause 0x01 (external-interrupt) via Vol VI
 * #0x520 InstallTrapHandler, then arms COMPARE = COUNT + quantum
 * and sets STATUS.IE so the timer starts firing. From this point
 * on, runaway CPU-bound tasks on the same CPU don't starve the
 * caller: every `quantum` cycles the handler fires, calls
 * TaskYield (deferred), and ERET picks the next runnable task.
 *
 * `quantum` is in cycles; the handler hardcodes 5000 internally
 * for the re-arm so this argument only sets the FIRST interval.
 * (Future libc could expose the quantum as a tunable in shared
 * state read by the handler.) */

extern void preempt_timer_handler(void);

void
task_install_preempt_timer(unsigned int quantum)
{
	/* InstallTrapHandler(R4=cause=1, R5=va=preempt_timer_handler) */
	asm volatile(
		"addiu r4, r0, 1\n"
		"la    r5, preempt_timer_handler\n"
		"call  #0x520\n"
		"nop"
		:
		:
		: "r2", "r3", "r4", "r5"
	);
	/* Arm: COMPARE = COUNT + quantum. */
	asm volatile(
		"lctrl r4, $5\n"
		"addu  r4, r4, %0\n"
		"sctrl $6, r4"
		:
		: "r"(quantum)
		: "r4"
	);
	/* Set STATUS.IE (bit 4), preserving mode bits. */
	asm volatile(
		"lctrl r4, $0\n"
		"ori   r4, r4, 0x10\n"
		"sctrl $0, r4"
		:
		:
		: "r4"
	);
}

/*
 * orx.c — Object RISC libc: load and run a .orx executable as a
 * child task on the calling CPU.
 *
 * The supervisor escape from "spawn programs only via linkbootd on
 * a separate CPU." orx_run(path) reads a .orx from the host
 * filesystem, ObjAllocs code/data/stack objects, copies the file's
 * text and data sections into them via temp VA mappings, and
 * TaskCreates a child to run the entry point. The child sees the
 * standard CONTRACT.md §2 layout — code at CODE_VA, data at
 * DATA_VA, stack at STACK_TOP — exactly as a fresh boot would
 * produce, so pcc-compiled C programs Just Work.
 *
 * Boot ABI required of callers
 * ----------------------------
 * orx_run depends on hf_init() having been called (it uses hf_open
 * / hf_read / hf_close to fetch the file). It does NOT depend on
 * task_init being called — orx_run manages task creation directly,
 * not through the task.c handle table. Callers can use orx_run and
 * the task.c API in the same program.
 *
 *     O7  = orx scratch object   (private; allocated on first orx_run)
 *
 * O7 must be free at orx_run entry — the only existing libc slot user
 * was linkboot.c, and a program that uses orx_run shouldn't also use
 * lb_spawn (the supervisor IS the spawn). The four working refs the
 * loader juggles (code / data / stack / task) live in the OR-typed
 * scratch at offsets 0 / 8 / 16 / 24, accessed by OREFLD/OREFST
 * with constant offsets.
 *
 * What's NOT done yet:
 *   - No concurrent invocations: orx_run is synchronous (load,
 *     spawn, wait, reap, return). Backgrounding awaits a future
 *     orx_load that returns a task_t handle.
 *   - Object cleanup is best-effort: if any step fails midway the
 *     successfully-allocated objects leak. The happy path frees
 *     everything; partial failures don't.
 */

#include "liborisc.h"

/* Section base VAs from CONTRACT.md §2. */
#define CODE_VA      0x00010000
#define DATA_VA      0x00040000
#define STACK_TOP    0x00200000

/* Loadable-module floor (Vol VI MapObject). Use VAs above 0x100000
 * for the temporary parent-side mappings we use to populate freshly
 * ObjAlloc'd code/data objects. Each load Unmaps after copying so
 * subsequent loads can reuse the same VAs. */
#define TEMP_CODE_VA 0x00300000
#define TEMP_DATA_VA 0x00400000

#define DEFAULT_STACK_SIZE 0x10000   /* 64 KiB — matches init_cpu */

#define TAG_CODE  0x4100
#define TAG_DATA  0x4102
#define TAG_STACK 0x4101

#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_X 0x04
#define CAP_V 0x10
#define CAP_C 0x40

/* Scratch object layout — offsets within the OR-typed object that
 * orx_run parks in O7. */
#define SLOT_CODE  0
#define SLOT_DATA  8
#define SLOT_STACK 16
#define SLOT_TASK  24
#define SCRATCH_BYTES 32

static unsigned int
beu32(const char *p)
{
	/* Explicit `& 0xff` after the load — pcc's orisc backend lowers
	 * the (unsigned char) cast to a signed `lb`, which sign-extends
	 * 0x80..0xFF bytes to 0xFFFFFF80.. and flips the high bits we
	 * OR-in afterwards. Masking after the load keeps each byte
	 * strictly 0..255. (TODO: fix the cast in pcc.) */
	return (((unsigned int)p[0] & 0xff) << 24)
	     | (((unsigned int)p[1] & 0xff) << 16)
	     | (((unsigned int)p[2] & 0xff) <<  8)
	     |  ((unsigned int)p[3] & 0xff);
}

static unsigned int
round4(unsigned int n)
{
	return (n + 3u) & ~3u;
}

/* --- scratch lifecycle ------------------------------------------- */

/* ObjAllocStore a 32-byte OR-typed object and park its ref in O7.
 * Returns the firmware status code (0 = OK). */
static int
orx_scratch_alloc(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, %1\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x106\n"            /* ObjAllocStore */
		"nop\n"
		"omov  o7, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(SCRATCH_BYTES), "i"(TAG_DATA), "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r2", "r3", "r4", "r5", "r6"
	);
	return status;
}

static int
orx_scratch_free(void)
{
	int status;
	asm volatile(
		"omov  o1, o7\n"
		"call  #0x101\n"            /* ObjFree */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2", "r3"
	);
	return status;
}

/* ObjAlloc a code/data/stack object of the requested size & tag,
 * with full caps, and OREFST the resulting ref into the named slot
 * of the scratch object. Returns 0 on success or the firmware
 * error code. */
static int
orx_alloc_into_slot(unsigned int size, unsigned int tag, unsigned int caps,
                    int slot)
{
	int status;
	/* The addus run in REVERSE order — pcc may have placed our input
	 * regs (%1/%2/%3) in r4/r5/r6 themselves, in which case writing
	 * `addu r4, %1, r0` first would clobber whatever %3 was if pcc
	 * picked r4 for it. Reading %3 → r6 first, then %2 → r5, then
	 * %1 → r4 leaves each pcc-chosen input register intact until
	 * after we've consumed it. (The same pattern recurs in every
	 * inline asm wrapper below — keep the addus reverse-ordered.) */
	asm volatile(
		"addu  r6, %3, r0\n"
		"addu  r5, %2, r0\n"
		"addu  r4, %1, r0\n"
		"call  #0x100\n"            /* ObjAlloc → O1 = ref */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(size), "r"(tag), "r"(caps)
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status != 0)
		return status;
	switch (slot) {
	case SLOT_CODE:  asm volatile("orefst o1, 0(o7)");  break;
	case SLOT_DATA:  asm volatile("orefst o1, 8(o7)");  break;
	case SLOT_STACK: asm volatile("orefst o1, 16(o7)"); break;
	case SLOT_TASK:  asm volatile("orefst o1, 24(o7)"); break;
	}
	return 0;
}

/* ObjFree the ref currently in the named slot. */
static int
orx_free_slot(int slot)
{
	int status;
	switch (slot) {
	case SLOT_CODE:  asm volatile("orefld o1, 0(o7)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 8(o7)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 16(o7)"); break;
	case SLOT_TASK:  asm volatile("orefld o1, 24(o7)"); break;
	}
	asm volatile(
		"call  #0x101\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r2", "r3"
	);
	return status;
}

/* Like orx_free_slot but uses ObjFreeDeferred (#0x107) with the
 * given drain delay (ms). The descriptor stays live during the
 * window so any in-flight OBJ_READ_REQs from oriscterm/hostfsd
 * against the soon-to-be-freed object can still be answered.
 *
 * Used at the end of orx_spawn for code/data/stack: the guest may
 * have buffered async SENDs whose receivers haven't issued reads
 * yet at the moment of TaskExit. */
static int
orx_freedef_slot(int slot, unsigned int delay_ms)
{
	int status;
	switch (slot) {
	case SLOT_CODE:  asm volatile("orefld o1, 0(o7)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 8(o7)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 16(o7)"); break;
	}
	asm volatile(
		"addu  r4, %1, r0\n"
		"call  #0x107\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(delay_ms)
		: "r2", "r3", "r4"
	);
	return status;
}

/* Load slot ref into O1, then MapObject(O1, va, 0, R+W, length). */
static int
orx_map_slot(int slot, unsigned int va, unsigned int length)
{
	int status;
	switch (slot) {
	case SLOT_CODE:  asm volatile("orefld o1, 0(o7)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 8(o7)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 16(o7)"); break;
	}
	/* Reverse order — see orx_alloc_into_slot's comment. */
	asm volatile(
		"addu  r7, %3, r0\n"
		"addiu r6, r0, %2\n"
		"addu  r5, r0, r0\n"
		"addu  r4, %1, r0\n"
		"call  #0x110\n"            /* MapObject */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(va), "i"(CAP_R | CAP_W), "r"(length)
		: "r2", "r3", "r4", "r5", "r6", "r7"
	);
	return status;
}

static int
orx_unmap(unsigned int va, unsigned int length)
{
	int status;
	asm volatile(
		"addu  r5, %2, r0\n"
		"addu  r4, %1, r0\n"
		"call  #0x111\n"            /* Unmap */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(va), "r"(length)
		: "r2", "r3", "r4", "r5"
	);
	return status;
}

/* Read `size` bytes from `fd` into the parent VA range starting at
 * `temp_va` (which must already be MapObject'd R+W). Buffers in
 * 1 KiB stack chunks; memcpy into the temp VA. */
static int
orx_read_into_va(int fd, unsigned int temp_va, unsigned int size)
{
	char buf[1024];
	unsigned int off = 0;
	while (off < size) {
		unsigned int want = size - off;
		if (want > sizeof(buf)) want = sizeof(buf);
		int got = hf_read(fd, buf, (int)want);
		if (got <= 0)
			return -1;
		memcpy((void *)(temp_va + off), buf, (unsigned int)got);
		off += (unsigned int)got;
	}
	return 0;
}

/* TaskCreate(O1=code, O2=stack, O3=data, R4=entry, R5=0) — leave
 * the resulting task ref in O1 for the caller to register into the
 * libc task table via task_register_o1. Restore O2/O3 from O11/O15
 * before returning so the caller's print_str etc. keep working.
 * Returns firmware status. */
static int
orx_task_create(unsigned int entry, int has_data)
{
	int status;
	if (has_data) {
		asm volatile(
			"orefld o1, 0(o7)\n"        /* code */
			"orefld o2, 16(o7)\n"       /* stack */
			"orefld o3, 8(o7)\n"        /* data */
			"addu  r4, %1, r0\n"
			"addu  r5, r0, r0\n"
			"call  #0x000\n"            /* TaskCreate → O1 = task */
			"nop\n"
			"omov  o2, o11\n"           /* restore parent's stack ref */
			"omov  o3, o15\n"           /* restore parent's data ref */
			"addu  %0, r2, r0"
			: "=r"(status)
			: "r"(entry)
			: "r2", "r3", "r4", "r5"
		);
	} else {
		asm volatile(
			"orefld o1, 0(o7)\n"
			"orefld o2, 16(o7)\n"
			"onull  o3\n"
			"addu  r4, %1, r0\n"
			"addu  r5, r0, r0\n"
			"call  #0x000\n"
			"nop\n"
			"omov  o2, o11\n"
			"omov  o3, o15\n"
			"addu  %0, r2, r0"
			: "=r"(status)
			: "r"(entry)
			: "r2", "r3", "r4", "r5"
		);
	}
	return status;
}

/*
 * orx_spawn — load .orx at `path`, TaskCreate it, register the
 * task in the libc task table (via task_register_o1), TaskResume,
 * and return the resulting task_t handle. The caller is responsible
 * for task_wait() / task_free() to harvest the exit code and reap
 * the descriptor. orx_run wraps spawn+wait+free for synchronous
 * use; backgrounded `run cmd &` calls orx_spawn and forgets.
 *
 * Requires task_init() to have been called (the libc task table
 * must exist in O12).
 *
 * Returns the task_t handle (0..TASK_MAX_CONCURRENT-1) on success,
 * or one of:
 *     -1  hf_open failed (file not found or no permission)
 *     -2  short header read or bad magic
 *     -3  header validation failure
 *     -4  ObjAlloc / MapObject / read failed during load
 *     -5  TaskCreate / task_register / TaskResume failed
 *     -6  libc task table is full
 */
task_t
orx_spawn(const char *path)
{
	char hdr[32];

	if (orx_scratch_alloc() != 0)
		return -4;

	int fd = hf_open(path, HF_O_RDONLY);
	if (fd < 0) { orx_scratch_free(); return -1; }

	if (hf_read(fd, hdr, 32) != 32) {
		hf_close(fd); orx_scratch_free(); return -2;
	}
	if (memcmp(hdr, "ORISC\x00\x00\x00", 8) != 0) {
		hf_close(fd); orx_scratch_free(); return -2;
	}
	unsigned int version    = beu32(hdr + 0x08);
	unsigned int entry      = beu32(hdr + 0x10);
	unsigned int text_size  = beu32(hdr + 0x14);
	unsigned int data_size  = beu32(hdr + 0x18);
	unsigned int stack_size = beu32(hdr + 0x1C);
	(void)version;
	if (version != 1 || (text_size & 3) != 0 || entry >= text_size) {
		hf_close(fd); orx_scratch_free(); return -3;
	}
	if (stack_size == 0)
		stack_size = DEFAULT_STACK_SIZE;

	unsigned int code_alloc = round4(text_size);
	unsigned int data_alloc = round4(data_size);
	int has_data = (data_size > 0);

	if (orx_alloc_into_slot(code_alloc, TAG_CODE,
				CAP_R | CAP_W | CAP_X | CAP_V | CAP_C,
				SLOT_CODE) != 0) {
		hf_close(fd); orx_scratch_free(); return -4;
	}
	if (orx_map_slot(SLOT_CODE, TEMP_CODE_VA, code_alloc) != 0) {
		orx_free_slot(SLOT_CODE);
		hf_close(fd); orx_scratch_free(); return -4;
	}
	if (orx_read_into_va(fd, TEMP_CODE_VA, text_size) != 0) {
		orx_unmap(TEMP_CODE_VA, code_alloc);
		orx_free_slot(SLOT_CODE);
		hf_close(fd); orx_scratch_free(); return -4;
	}
	orx_unmap(TEMP_CODE_VA, code_alloc);

	if (has_data) {
		if (orx_alloc_into_slot(data_alloc, TAG_DATA,
					CAP_R | CAP_W | CAP_V | CAP_C,
					SLOT_DATA) != 0) {
			orx_free_slot(SLOT_CODE);
			hf_close(fd); orx_scratch_free(); return -4;
		}
		if (orx_map_slot(SLOT_DATA, TEMP_DATA_VA, data_alloc) != 0) {
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			hf_close(fd); orx_scratch_free(); return -4;
		}
		if (orx_read_into_va(fd, TEMP_DATA_VA, data_size) != 0) {
			orx_unmap(TEMP_DATA_VA, data_alloc);
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			hf_close(fd); orx_scratch_free(); return -4;
		}
		orx_unmap(TEMP_DATA_VA, data_alloc);
	}
	hf_close(fd);

	if (orx_alloc_into_slot(stack_size, TAG_STACK,
				CAP_R | CAP_W | CAP_V | CAP_C,
				SLOT_STACK) != 0) {
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		orx_scratch_free(); return -4;
	}

	/* TaskCreate leaves the new task ref in O1. Hand it straight to
	 * the libc table via task_register_o1 so the caller can use the
	 * standard task_wait / task_free API. */
	if (orx_task_create(entry, has_data) != 0) {
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		orx_scratch_free(); return -5;
	}
	task_t t = task_register_o1();
	if (t < 0) {
		/* Task table full. The task descriptor is now orphaned —
		 * we can't reach it via the libc API, but it's harmless
		 * (the task hasn't been resumed; it'll never run). */
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		orx_scratch_free(); return -6;
	}
	if (task_resume(t) != 0) {
		task_free(t);
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		orx_scratch_free(); return -5;
	}

	/* Schedule deferred frees of the loaded code/data/stack with a
	 * generous drain window. The descriptors stay live until the
	 * deadline elapses, which gives the guest plenty of run-time and
	 * the receiver(s) of any async SENDs (oriscterm, hostfsd) time to
	 * read the bytes before the storage disappears. The 30-second
	 * window is the practical upper bound on how long an interactive
	 * guest is expected to run; longer-lived guests would have their
	 * data pulled out from under them and would need a different
	 * model (an explicit unload primitive that runs after task_wait).
	 * The trailing scratch object isn't shared with anyone — drop
	 * immediately. */
	orx_freedef_slot(SLOT_CODE, 30000);
	if (has_data)
		orx_freedef_slot(SLOT_DATA, 30000);
	orx_freedef_slot(SLOT_STACK, 30000);
	orx_scratch_free();
	return t;
}

/*
 * orx_run — the synchronous wrapper. Spawns the guest, waits for
 * exit, reaps the task descriptor, returns the exit code.
 *
 * Returns 0..255 on success or the negative status code from
 * orx_spawn (see above).
 */
int
orx_run(const char *path)
{
	task_t t = orx_spawn(path);
	if (t < 0)
		return t;
	int code = task_wait(t);
	task_free(t);
	if (code < 0)
		return -5;
	return code;
}

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

/* O7-parked persistent state object layout. The first 24 bytes are
 * scratch slots used by orx_spawn during the load (overwritten on
 * each call); bytes 24..407 hold a per-task manifest of the loaded
 * code/data/stack refs, indexed by the task_t handle the libc table
 * assigned. orx_unload(t) reads manifest[t] to know which objects
 * to ObjFreeDeferred when the guest exits.
 *
 *   bytes      meaning
 *   0    .. 7  scratch SLOT_CODE   (current spawn's loaded code ref)
 *   8    ..15  scratch SLOT_DATA
 *   16   ..23  scratch SLOT_STACK
 *   24+t*24+0  manifest[t].code    (1 ≤ t ≤ TASK_MAX_CONCURRENT)
 *   24+t*24+8  manifest[t].data
 *   24+t*24+16 manifest[t].stack
 *
 * Total bytes: 24 + 16*24 = 408. */
#define SLOT_CODE      0
#define SLOT_DATA      8
#define SLOT_STACK    16
#define MANIFEST_BASE 24
#define STATE_BYTES   408

/* Drain delay handed to ObjFreeDeferred from orx_unload — chosen
 * comfortably larger than the longest plausible OBJ_READ_REQ
 * round-trip from a Tk-mediated oriscterm SEND, but short enough
 * that exiting the shell doesn't leave huge frees pending. */
#define UNLOAD_DRAIN_MS 1500

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

/* --- persistent state lifecycle --------------------------------- */

/* orx_state_initialized is the lazy-init guard: 0 = state object
 * not yet ObjAllocStore'd, 1 = O7 holds the persistent state ref.
 * Lives in regular int memory (the .data segment). */
static int orx_state_initialized;

/* ObjAllocStore the 408-byte persistent state and park in O7. Lazy
 * — returns OK immediately on subsequent calls without re-allocating
 * (the same object is reused across all orx_spawn invocations). */
static int
orx_state_init(void)
{
	if (orx_state_initialized)
		return 0;
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
		: "i"(STATE_BYTES), "i"(TAG_DATA), "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status == 0)
		orx_state_initialized = 1;
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
	}
	return 0;
}

/* ObjFree the ref currently in the named slot — used in error
 * paths within orx_spawn to release partially-allocated objects
 * before bailing. The persistent state object itself isn't freed
 * (it's reused across all orx_spawn invocations). */
static int
orx_free_slot(int slot)
{
	int status;
	switch (slot) {
	case SLOT_CODE:  asm volatile("orefld o1, 0(o7)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 8(o7)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 16(o7)"); break;
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

/* --- per-task manifest helpers ----------------------------------
 *
 * The persistent state's manifest area holds three OR refs per
 * task slot: code at +0, data at +8, stack at +16, with each slot
 * 24 bytes wide and the table starting at byte MANIFEST_BASE = 24.
 *
 * OREFLD/OREFST take a constant 16-bit offset, so a runtime task_t
 * has to dispatch through a switch. We do all three refs of one
 * slot in a single asm block per case to keep the source compact —
 * 16 cases × 3 OREF instructions per case is mechanical but tidy. */

/* OREFST O1, O2, O3 → manifest[t].{code, data, stack}. Caller
 * must have populated O1/O2/O3 with the refs to save. */
static void
manifest_save(int t)
{
	switch (t) {
	case  0: asm volatile("orefst o1, 24(o7)\norefst o2, 32(o7)\norefst o3, 40(o7)"); break;
	case  1: asm volatile("orefst o1, 48(o7)\norefst o2, 56(o7)\norefst o3, 64(o7)"); break;
	case  2: asm volatile("orefst o1, 72(o7)\norefst o2, 80(o7)\norefst o3, 88(o7)"); break;
	case  3: asm volatile("orefst o1, 96(o7)\norefst o2, 104(o7)\norefst o3, 112(o7)"); break;
	case  4: asm volatile("orefst o1, 120(o7)\norefst o2, 128(o7)\norefst o3, 136(o7)"); break;
	case  5: asm volatile("orefst o1, 144(o7)\norefst o2, 152(o7)\norefst o3, 160(o7)"); break;
	case  6: asm volatile("orefst o1, 168(o7)\norefst o2, 176(o7)\norefst o3, 184(o7)"); break;
	case  7: asm volatile("orefst o1, 192(o7)\norefst o2, 200(o7)\norefst o3, 208(o7)"); break;
	case  8: asm volatile("orefst o1, 216(o7)\norefst o2, 224(o7)\norefst o3, 232(o7)"); break;
	case  9: asm volatile("orefst o1, 240(o7)\norefst o2, 248(o7)\norefst o3, 256(o7)"); break;
	case 10: asm volatile("orefst o1, 264(o7)\norefst o2, 272(o7)\norefst o3, 280(o7)"); break;
	case 11: asm volatile("orefst o1, 288(o7)\norefst o2, 296(o7)\norefst o3, 304(o7)"); break;
	case 12: asm volatile("orefst o1, 312(o7)\norefst o2, 320(o7)\norefst o3, 328(o7)"); break;
	case 13: asm volatile("orefst o1, 336(o7)\norefst o2, 344(o7)\norefst o3, 352(o7)"); break;
	case 14: asm volatile("orefst o1, 360(o7)\norefst o2, 368(o7)\norefst o3, 376(o7)"); break;
	case 15: asm volatile("orefst o1, 384(o7)\norefst o2, 392(o7)\norefst o3, 400(o7)"); break;
	}
}

/* OREFLD manifest[t].{code, data, stack} → O1, O2, O3. */
static void
manifest_load(int t)
{
	switch (t) {
	case  0: asm volatile("orefld o1, 24(o7)\norefld o2, 32(o7)\norefld o3, 40(o7)"); break;
	case  1: asm volatile("orefld o1, 48(o7)\norefld o2, 56(o7)\norefld o3, 64(o7)"); break;
	case  2: asm volatile("orefld o1, 72(o7)\norefld o2, 80(o7)\norefld o3, 88(o7)"); break;
	case  3: asm volatile("orefld o1, 96(o7)\norefld o2, 104(o7)\norefld o3, 112(o7)"); break;
	case  4: asm volatile("orefld o1, 120(o7)\norefld o2, 128(o7)\norefld o3, 136(o7)"); break;
	case  5: asm volatile("orefld o1, 144(o7)\norefld o2, 152(o7)\norefld o3, 160(o7)"); break;
	case  6: asm volatile("orefld o1, 168(o7)\norefld o2, 176(o7)\norefld o3, 184(o7)"); break;
	case  7: asm volatile("orefld o1, 192(o7)\norefld o2, 200(o7)\norefld o3, 208(o7)"); break;
	case  8: asm volatile("orefld o1, 216(o7)\norefld o2, 224(o7)\norefld o3, 232(o7)"); break;
	case  9: asm volatile("orefld o1, 240(o7)\norefld o2, 248(o7)\norefld o3, 256(o7)"); break;
	case 10: asm volatile("orefld o1, 264(o7)\norefld o2, 272(o7)\norefld o3, 280(o7)"); break;
	case 11: asm volatile("orefld o1, 288(o7)\norefld o2, 296(o7)\norefld o3, 304(o7)"); break;
	case 12: asm volatile("orefld o1, 312(o7)\norefld o2, 320(o7)\norefld o3, 328(o7)"); break;
	case 13: asm volatile("orefld o1, 336(o7)\norefld o2, 344(o7)\norefld o3, 352(o7)"); break;
	case 14: asm volatile("orefld o1, 360(o7)\norefld o2, 368(o7)\norefld o3, 376(o7)"); break;
	case 15: asm volatile("orefld o1, 384(o7)\norefld o2, 392(o7)\norefld o3, 400(o7)"); break;
	}
}

/* OREFST O0 (null) into manifest[t].{code, data, stack}. */
static void
manifest_clear(int t)
{
	switch (t) {
	case  0: asm volatile("orefst o0, 24(o7)\norefst o0, 32(o7)\norefst o0, 40(o7)"); break;
	case  1: asm volatile("orefst o0, 48(o7)\norefst o0, 56(o7)\norefst o0, 64(o7)"); break;
	case  2: asm volatile("orefst o0, 72(o7)\norefst o0, 80(o7)\norefst o0, 88(o7)"); break;
	case  3: asm volatile("orefst o0, 96(o7)\norefst o0, 104(o7)\norefst o0, 112(o7)"); break;
	case  4: asm volatile("orefst o0, 120(o7)\norefst o0, 128(o7)\norefst o0, 136(o7)"); break;
	case  5: asm volatile("orefst o0, 144(o7)\norefst o0, 152(o7)\norefst o0, 160(o7)"); break;
	case  6: asm volatile("orefst o0, 168(o7)\norefst o0, 176(o7)\norefst o0, 184(o7)"); break;
	case  7: asm volatile("orefst o0, 192(o7)\norefst o0, 200(o7)\norefst o0, 208(o7)"); break;
	case  8: asm volatile("orefst o0, 216(o7)\norefst o0, 224(o7)\norefst o0, 232(o7)"); break;
	case  9: asm volatile("orefst o0, 240(o7)\norefst o0, 248(o7)\norefst o0, 256(o7)"); break;
	case 10: asm volatile("orefst o0, 264(o7)\norefst o0, 272(o7)\norefst o0, 280(o7)"); break;
	case 11: asm volatile("orefst o0, 288(o7)\norefst o0, 296(o7)\norefst o0, 304(o7)"); break;
	case 12: asm volatile("orefst o0, 312(o7)\norefst o0, 320(o7)\norefst o0, 328(o7)"); break;
	case 13: asm volatile("orefst o0, 336(o7)\norefst o0, 344(o7)\norefst o0, 352(o7)"); break;
	case 14: asm volatile("orefst o0, 360(o7)\norefst o0, 368(o7)\norefst o0, 376(o7)"); break;
	case 15: asm volatile("orefst o0, 384(o7)\norefst o0, 392(o7)\norefst o0, 400(o7)"); break;
	}
}

/* Issue ObjFreeDeferred(O1, UNLOAD_DRAIN_MS) and ignore the status.
 * Used by orx_unload across all three of code/data/stack — null
 * refs (from a non-orx-spawned task whose manifest entries are
 * empty) would simply EFAULT, which we want to swallow silently. */
static void
freedef_o1(void)
{
	asm volatile(
		"addiu r4, r0, %0\n"
		"call  #0x107\n"
		"nop"
		: : "i"(UNLOAD_DRAIN_MS) : "r2", "r3", "r4"
	);
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

	if (orx_state_init() != 0)
		return -4;

	int fd = hf_open(path, HF_O_RDONLY);
	if (fd < 0) { return -1; }

	if (hf_read(fd, hdr, 32) != 32) {
		hf_close(fd); return -2;
	}
	if (memcmp(hdr, "ORISC\x00\x00\x00", 8) != 0) {
		hf_close(fd); return -2;
	}
	unsigned int version    = beu32(hdr + 0x08);
	unsigned int entry      = beu32(hdr + 0x10);
	unsigned int text_size  = beu32(hdr + 0x14);
	unsigned int data_size  = beu32(hdr + 0x18);
	unsigned int stack_size = beu32(hdr + 0x1C);
	(void)version;
	if (version != 1 || (text_size & 3) != 0 || entry >= text_size) {
		hf_close(fd); return -3;
	}
	if (stack_size == 0)
		stack_size = DEFAULT_STACK_SIZE;

	unsigned int code_alloc = round4(text_size);
	unsigned int data_alloc = round4(data_size);
	int has_data = (data_size > 0);

	if (orx_alloc_into_slot(code_alloc, TAG_CODE,
				CAP_R | CAP_W | CAP_X | CAP_V | CAP_C,
				SLOT_CODE) != 0) {
		hf_close(fd); return -4;
	}
	if (orx_map_slot(SLOT_CODE, TEMP_CODE_VA, code_alloc) != 0) {
		orx_free_slot(SLOT_CODE);
		hf_close(fd); return -4;
	}
	if (orx_read_into_va(fd, TEMP_CODE_VA, text_size) != 0) {
		orx_unmap(TEMP_CODE_VA, code_alloc);
		orx_free_slot(SLOT_CODE);
		hf_close(fd); return -4;
	}
	orx_unmap(TEMP_CODE_VA, code_alloc);

	if (has_data) {
		if (orx_alloc_into_slot(data_alloc, TAG_DATA,
					CAP_R | CAP_W | CAP_V | CAP_C,
					SLOT_DATA) != 0) {
			orx_free_slot(SLOT_CODE);
			hf_close(fd); return -4;
		}
		if (orx_map_slot(SLOT_DATA, TEMP_DATA_VA, data_alloc) != 0) {
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			hf_close(fd); return -4;
		}
		if (orx_read_into_va(fd, TEMP_DATA_VA, data_size) != 0) {
			orx_unmap(TEMP_DATA_VA, data_alloc);
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			hf_close(fd); return -4;
		}
		orx_unmap(TEMP_DATA_VA, data_alloc);
	}
	hf_close(fd);

	if (orx_alloc_into_slot(stack_size, TAG_STACK,
				CAP_R | CAP_W | CAP_V | CAP_C,
				SLOT_STACK) != 0) {
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		return -4;
	}

	/* TaskCreate leaves the new task ref in O1. Hand it straight to
	 * the libc table via task_register_o1 so the caller can use the
	 * standard task_wait / task_free API. */
	if (orx_task_create(entry, has_data) != 0) {
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		return -5;
	}
	task_t t = task_register_o1();
	if (t < 0) {
		/* Task table full. The task descriptor is now orphaned —
		 * we can't reach it via the libc API, but it's harmless
		 * (the task hasn't been resumed; it'll never run). */
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		return -6;
	}
	if (task_resume(t) != 0) {
		task_free(t);
		orx_free_slot(SLOT_STACK);
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		return -5;
	}

	/* Persist the loaded refs into manifest[t] so orx_unload(t) can
	 * find them later (after task_wait has confirmed the guest is
	 * done). OREFLD scratch slots into O1/O2/O3 first, then a single
	 * manifest_save with three OREFSTs — clean OPR shuffle, no
	 * intermediate spills. */
	if (has_data) {
		asm volatile(
			"orefld o1, 0(o7)\n"
			"orefld o2, 8(o7)\n"
			"orefld o3, 16(o7)"
		);
	} else {
		asm volatile(
			"orefld o1, 0(o7)\n"
			"onull  o2\n"
			"orefld o3, 16(o7)"
		);
	}
	manifest_save(t);
	return t;
}

/*
 * orx_unload — wait for an orx-spawned task to exit, schedule
 * deferred frees of its loaded code/data/stack, and reap the task
 * descriptor. Returns the guest's exit code (0..255), or a
 * negative error from task_wait.
 *
 * The drain timer for the deferred frees starts WHEN orx_unload
 * is called, not at spawn time — by then task_wait has confirmed
 * the guest exited, so UNLOAD_DRAIN_MS (1500 ms) is plenty for any
 * lingering OBJ_READ_REQs to drain. Long-lived guests are no
 * longer at risk of having their data segment pulled out mid-run.
 *
 * Safe to call on a task_t whose manifest is empty (e.g., a
 * non-orx-spawned task created by task_spawn): the manifest
 * entries are null, ObjFreeDeferred returns EFAULT for each, and
 * we swallow it silently. The shell's cmd_wait calls this
 * uniformly so no caller has to know which API spawned the task.
 */
int
orx_unload(task_t t)
{
	int code = task_wait(t);
	if (code < 0)
		return code;
	if (!orx_state_initialized) {
		/* No state object — nothing to OREFLD. Just task_free. */
		task_free(t);
		return code;
	}
	/* OREFLD manifest[t].{code,data,stack} → O1/O2/O3, then
	 * ObjFreeDeferred each in turn (omov o1, oN to swap operands). */
	manifest_load(t);
	freedef_o1();                       /* O1 = code */
	asm volatile("omov o1, o2");
	freedef_o1();                       /* O1 = data */
	asm volatile("omov o1, o3");
	freedef_o1();                       /* O1 = stack */
	manifest_clear(t);
	task_free(t);
	return code;
}

/*
 * orx_run — the synchronous wrapper. Spawns the guest, waits for
 * exit (via orx_unload, which also schedules cleanup), returns the
 * exit code.
 *
 * Returns 0..255 on success or the negative status code from
 * orx_spawn / orx_unload.
 */
int
orx_run(const char *path)
{
	task_t t = orx_spawn(path);
	if (t < 0)
		return t;
	return orx_unload(t);
}

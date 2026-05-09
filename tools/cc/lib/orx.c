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
 * / hf_read / hf_close to fetch the file) AND on task_init() having
 * been called: orx's persistent scratch + per-task manifest now
 * lives at the back end of task.c's objstore in O12, past the
 * 128-byte task table.
 *
 *     O12 (offset >= 128) = orx scratch + manifest
 *
 * Used to live in O7 in its own object, but Phase 38's grid service
 * ref also wanted O7. Co-located with the task table to free the
 * slot. The four working refs the loader juggles (code / data /
 * stack / task) live in the OR-typed scratch at offsets 0/8/16/24
 * past the task-table base.
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
/* Long-lived mapping in the PARENT for the shared argv buffer. Once
 * orx_argv_alloc establishes it, we leave it mapped forever — the
 * per-spawn cost shrinks to a memcpy (no MapObject/Unmap pair). The
 * VA sits above all the per-load temps so it doesn't collide. */
#define ARGS_PARENT_VA 0x00500000

#define DEFAULT_STACK_SIZE 0x10000   /* 64 KiB — matches init_cpu */

#define TAG_CODE  0x4100
#define TAG_DATA  0x4102
#define TAG_STACK 0x4101

#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_X 0x04
#define CAP_V 0x10
#define CAP_C 0x40

/* Persistent state layout. We append to the task-table objstore at
 * O12 — task.c oversizes the allocation by ORX_STATE_BYTES so we
 * can pick up at byte offset 128 (= TABLE_BYTES). Within our
 * region: the first 24 bytes are scratch slots used during a load
 * (overwritten on each call), and bytes 24..407 hold a per-task
 * manifest of the loaded code/data/stack refs, indexed by the
 * libc task_t handle. orx_unload(t) reads manifest[t] to know
 * which objects to ObjFreeDeferred on exit.
 *
 * The actual offsets baked into the orefst/orefld switches below
 * are pre-shifted by 128 (e.g., what would have been "0(o7)" is
 * "128(o12)"). The constants below are kept for documentation and
 * for the SLOT enum that refers to per-task scratch positions. */
#define SLOT_CODE      0
#define SLOT_DATA      8
#define SLOT_STACK    16
#define MANIFEST_BASE 24

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

/* orx's persistent state is now embedded inside the task-table
 * objstore (O12, allocated by task_init). All offsets in the
 * orefst/orefld switches below are pre-shifted by TABLE_BYTES (128)
 * so they land past the libc task table. orx_state_init is kept
 * for ABI compatibility — it just verifies task_init has run by
 * checking O12 is non-null — but does not allocate any more.
 *
 * Why share the slot: O7 used to be ours, but Phase 38's grid
 * service ref also wants O7 (oriscterm idx 3). Co-locating with
 * the task table frees O7 without needing a new OPR slot. */
static int
orx_state_init(void)
{
	/* No-op now that task_init owns the storage. Kept as a function
	 * (rather than removed) so other libc translation units can
	 * still link against it without churn. */
	return 0;
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
	case SLOT_CODE:  asm volatile("orefst o1, 128(o12)");  break;
	case SLOT_DATA:  asm volatile("orefst o1, 136(o12)");  break;
	case SLOT_STACK: asm volatile("orefst o1, 144(o12)"); break;
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
	case SLOT_CODE:  asm volatile("orefld o1, 128(o12)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 136(o12)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 144(o12)"); break;
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
	case  0: asm volatile("orefst o1, 152(o12)\norefst o2, 160(o12)\norefst o3, 168(o12)"); break;
	case  1: asm volatile("orefst o1, 176(o12)\norefst o2, 184(o12)\norefst o3, 192(o12)"); break;
	case  2: asm volatile("orefst o1, 200(o12)\norefst o2, 208(o12)\norefst o3, 216(o12)"); break;
	case  3: asm volatile("orefst o1, 224(o12)\norefst o2, 232(o12)\norefst o3, 240(o12)"); break;
	case  4: asm volatile("orefst o1, 248(o12)\norefst o2, 256(o12)\norefst o3, 264(o12)"); break;
	case  5: asm volatile("orefst o1, 272(o12)\norefst o2, 280(o12)\norefst o3, 288(o12)"); break;
	case  6: asm volatile("orefst o1, 296(o12)\norefst o2, 304(o12)\norefst o3, 312(o12)"); break;
	case  7: asm volatile("orefst o1, 320(o12)\norefst o2, 328(o12)\norefst o3, 336(o12)"); break;
	case  8: asm volatile("orefst o1, 344(o12)\norefst o2, 352(o12)\norefst o3, 360(o12)"); break;
	case  9: asm volatile("orefst o1, 368(o12)\norefst o2, 376(o12)\norefst o3, 384(o12)"); break;
	case 10: asm volatile("orefst o1, 392(o12)\norefst o2, 400(o12)\norefst o3, 408(o12)"); break;
	case 11: asm volatile("orefst o1, 416(o12)\norefst o2, 424(o12)\norefst o3, 432(o12)"); break;
	case 12: asm volatile("orefst o1, 440(o12)\norefst o2, 448(o12)\norefst o3, 456(o12)"); break;
	case 13: asm volatile("orefst o1, 464(o12)\norefst o2, 472(o12)\norefst o3, 480(o12)"); break;
	case 14: asm volatile("orefst o1, 488(o12)\norefst o2, 496(o12)\norefst o3, 504(o12)"); break;
	case 15: asm volatile("orefst o1, 512(o12)\norefst o2, 520(o12)\norefst o3, 528(o12)"); break;
	}
}

/* OREFLD manifest[t].{code, data, stack} → O1, O2, O3. */
static void
manifest_load(int t)
{
	switch (t) {
	case  0: asm volatile("orefld o1, 152(o12)\norefld o2, 160(o12)\norefld o3, 168(o12)"); break;
	case  1: asm volatile("orefld o1, 176(o12)\norefld o2, 184(o12)\norefld o3, 192(o12)"); break;
	case  2: asm volatile("orefld o1, 200(o12)\norefld o2, 208(o12)\norefld o3, 216(o12)"); break;
	case  3: asm volatile("orefld o1, 224(o12)\norefld o2, 232(o12)\norefld o3, 240(o12)"); break;
	case  4: asm volatile("orefld o1, 248(o12)\norefld o2, 256(o12)\norefld o3, 264(o12)"); break;
	case  5: asm volatile("orefld o1, 272(o12)\norefld o2, 280(o12)\norefld o3, 288(o12)"); break;
	case  6: asm volatile("orefld o1, 296(o12)\norefld o2, 304(o12)\norefld o3, 312(o12)"); break;
	case  7: asm volatile("orefld o1, 320(o12)\norefld o2, 328(o12)\norefld o3, 336(o12)"); break;
	case  8: asm volatile("orefld o1, 344(o12)\norefld o2, 352(o12)\norefld o3, 360(o12)"); break;
	case  9: asm volatile("orefld o1, 368(o12)\norefld o2, 376(o12)\norefld o3, 384(o12)"); break;
	case 10: asm volatile("orefld o1, 392(o12)\norefld o2, 400(o12)\norefld o3, 408(o12)"); break;
	case 11: asm volatile("orefld o1, 416(o12)\norefld o2, 424(o12)\norefld o3, 432(o12)"); break;
	case 12: asm volatile("orefld o1, 440(o12)\norefld o2, 448(o12)\norefld o3, 456(o12)"); break;
	case 13: asm volatile("orefld o1, 464(o12)\norefld o2, 472(o12)\norefld o3, 480(o12)"); break;
	case 14: asm volatile("orefld o1, 488(o12)\norefld o2, 496(o12)\norefld o3, 504(o12)"); break;
	case 15: asm volatile("orefld o1, 512(o12)\norefld o2, 520(o12)\norefld o3, 528(o12)"); break;
	}
}

/* OREFST O0 (null) into manifest[t].{code, data, stack}. */
static void
manifest_clear(int t)
{
	switch (t) {
	case  0: asm volatile("orefst o0, 152(o12)\norefst o0, 160(o12)\norefst o0, 168(o12)"); break;
	case  1: asm volatile("orefst o0, 176(o12)\norefst o0, 184(o12)\norefst o0, 192(o12)"); break;
	case  2: asm volatile("orefst o0, 200(o12)\norefst o0, 208(o12)\norefst o0, 216(o12)"); break;
	case  3: asm volatile("orefst o0, 224(o12)\norefst o0, 232(o12)\norefst o0, 240(o12)"); break;
	case  4: asm volatile("orefst o0, 248(o12)\norefst o0, 256(o12)\norefst o0, 264(o12)"); break;
	case  5: asm volatile("orefst o0, 272(o12)\norefst o0, 280(o12)\norefst o0, 288(o12)"); break;
	case  6: asm volatile("orefst o0, 296(o12)\norefst o0, 304(o12)\norefst o0, 312(o12)"); break;
	case  7: asm volatile("orefst o0, 320(o12)\norefst o0, 328(o12)\norefst o0, 336(o12)"); break;
	case  8: asm volatile("orefst o0, 344(o12)\norefst o0, 352(o12)\norefst o0, 360(o12)"); break;
	case  9: asm volatile("orefst o0, 368(o12)\norefst o0, 376(o12)\norefst o0, 384(o12)"); break;
	case 10: asm volatile("orefst o0, 392(o12)\norefst o0, 400(o12)\norefst o0, 408(o12)"); break;
	case 11: asm volatile("orefst o0, 416(o12)\norefst o0, 424(o12)\norefst o0, 432(o12)"); break;
	case 12: asm volatile("orefst o0, 440(o12)\norefst o0, 448(o12)\norefst o0, 456(o12)"); break;
	case 13: asm volatile("orefst o0, 464(o12)\norefst o0, 472(o12)\norefst o0, 480(o12)"); break;
	case 14: asm volatile("orefst o0, 488(o12)\norefst o0, 496(o12)\norefst o0, 504(o12)"); break;
	case 15: asm volatile("orefst o0, 512(o12)\norefst o0, 520(o12)\norefst o0, 528(o12)"); break;
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
	case SLOT_CODE:  asm volatile("orefld o1, 128(o12)");  break;
	case SLOT_DATA:  asm volatile("orefld o1, 136(o12)");  break;
	case SLOT_STACK: asm volatile("orefld o1, 144(o12)"); break;
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
		int got = vfs_read(fd, buf, (int)want);
		if (got <= 0)
			return -1;
		memcpy((void *)(temp_va + off), buf, (unsigned int)got);
		off += (unsigned int)got;
	}
	return 0;
}

/* Args object: a 256-byte TAG_DATA buffer holding a NUL-terminated
 * args string the spawned program will see at ARGV_VA. We allocate
 * it once (lazy, on first orx_spawn) and reuse the same object for
 * every subsequent spawn — the contents are overwritten per call.
 *
 * Why one shared object instead of one per spawn:
 *
 *   - Per-spawn ObjAlloc grows the descriptor table forever (we
 *     can't easily free until the child exits, and even then we'd
 *     need a per-task manifest slot).
 *   - The race "child still reading args while parent overwrites"
 *     doesn't happen in practice: the child copies args to its
 *     own buffer right after term_init / hf_init in main(), well
 *     before the parent could spawn another guest concurrently.
 *
 * The ref lives in orx-state ORX_SLOT_ARGV (orx-region offset 408
 * → o12 offset 128 + 408 = 536). The lazy-init guard is just
 * "is the slot null?" — orefst-checked via OISN. */
#define ARGV_BUF_SIZE 256

/* Lay out the args buffer at `va`: `args\0cwd\0`. Args first to
 * preserve `program_args() = (char *)ARGV_VA` (dereferencing it
 * still reads the args string, NUL-terminated). cwd lives past
 * the args terminator so `program_cwd()` can find it by walking
 * the buffer past one NUL. Either may be NULL/"". Receiving va as
 * a register dodges pcc's `la r,N` lowering for cast-from-literal. */
static void
orx_argv_copy(unsigned int va, const char *args, const char *cwd)
{
	char *dst = (char *)va;
	unsigned int i = 0;
	if (args) {
		while (i + 1 < ARGV_BUF_SIZE && args[i]) {
			dst[i] = args[i];
			i++;
		}
	}
	dst[i++] = '\0';
	unsigned int j = 0;
	if (cwd) {
		while (i + 1 < ARGV_BUF_SIZE && cwd[j]) {
			dst[i++] = cwd[j++];
		}
	}
	dst[i] = '\0';
}

/* ObjAlloc the shared argv object and OREFST its ref into
 * ORX_SLOT_ARGV. Returns firmware status. The OREFST happens
 * inside the same asm block as the call so no compiler-emitted
 * instructions can land between (which is fine for GPRs but pcc
 * makes no guarantees about OPR preservation across its lowering). */
static int
orx_argv_alloc(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, %1\n"
		"addiu r5, r0, %2\n"
		"addiu r6, r0, %3\n"
		"call  #0x100\n"
		"nop\n"
		"orefst o1, 536(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(ARGV_BUF_SIZE), "i"(TAG_DATA),
		  "i"(CAP_R | CAP_W | CAP_V | CAP_C)
		: "r2", "r3", "r4", "r5", "r6"
	);
	return status;
}

/* MapObject(O1=argv, va=ARGS_PARENT_VA, 0, R+W, ARGV_BUF_SIZE).
 * Establishes the long-lived parent-side mapping; called once after
 * orx_argv_alloc. Separated from alloc so the alloc fast-path
 * stays a single-asm-block guarantee, matching orx_alloc_into_slot. */
static int
orx_argv_map(void)
{
	int status;
	asm volatile(
		"orefld o1, 536(o12)\n"
		"addiu r7, r0, %2\n"
		"addiu r6, r0, %1\n"
		"addu  r5, r0, r0\n"
		"lui   r4, %3\n"
		"call  #0x110\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(CAP_R | CAP_W),
		  "i"(ARGV_BUF_SIZE),
		  "i"(ARGS_PARENT_VA >> 16)
		: "r2", "r3", "r4", "r5", "r6", "r7"
	);
	return status;
}

/* Is the argv slot still null? (i.e. needs lazy alloc.) */
static int
orx_argv_is_null(void)
{
	int isn;
	asm volatile(
		"orefld o1, 536(o12)\n"
		"oisn   %0, o1"
		: "=r"(isn)
		:
		: "r1"
	);
	return isn;
}

/* End-to-end: on first call, ObjAlloc the shared argv object and
 * MapObject it R+W at ARGS_PARENT_VA in the parent (a one-shot
 * cost). On every subsequent call: just memcpy `args` into the
 * already-mapped buffer — no firmware primitives, no per-spawn
 * map/unmap pair. The ref persists in ORX_SLOT_ARGV;
 * orx_task_create OREFLDs it into O4 just before TaskCreate, so the
 * firmware maps the same object at ARGV_VA in the child.
 *
 * Multi-spawn callers (shells) should call orx_init() at boot so
 * the alloc + map cost lands BEFORE the spawn path. Single-shot
 * programs can rely on the lazy alloc here. */
static int
orx_setup_args(const char *args, const char *cwd)
{
	if (orx_argv_is_null()) {
		int status = orx_argv_alloc();
		if (status != 0) return status;
	}
	orx_argv_copy((unsigned int)ARGS_PARENT_VA, args, cwd);
	return 0;
}

/* Public boot-time initialization: pre-allocate + persistently map
 * the shared argv buffer. Optional — orx_setup_args lazily allocates
 * on first spawn if this wasn't called — but recommended for any
 * program that spawns multiple children, because moving the
 * one-time ObjAlloc + MapObject pair out of the first orx_run keeps
 * the spawn path predictable (just a memcpy). Idempotent: safe to
 * call more than once. */
int
orx_init(void)
{
	if (orx_argv_is_null()) {
		int s = orx_argv_alloc();
		if (s != 0) return s;
	}
	return orx_argv_map();
}

/* Byte offsets within O12 of the slots orx_task_create reads to
 * inject custom OPRs into the child.
 *
 * ORX_SLOT_CHILD_O8 (Phase 45a) — the cap to put in O8 just before
 *   TaskCreate (used by the supervisor so spawned tasks' task_init
 *   harvests its sub-cap).
 * ORX_SLOT_CHILD_O5/O6/O7 (Phase 49) — terminal-pass-through:
 *   console/keyboard/grid sub-caps, set by the supervisor when
 *   servicing a relayed spawn whose source CPU's terminal differs
 *   from ours. Null = no override; child inherits parent's OPRs as
 *   before. The receiver dir-walks /sys/term/<source>/* to populate
 *   these, so a `run cmd` from a shell on CPU 0 round-robin'd to
 *   CPU 1 still routes its term_print/term_getkey to terminal 0.
 *
 * Each CHILD_O* has a paired O*_SAVE slot — transient stash for the
 * parent's OPR across the swap window inside orx_task_create. */
#define ORX_SLOT_CHILD_O8_OFFSET 560
#define ORX_SLOT_O8_SAVE_OFFSET  568
#define ORX_SLOT_CHILD_O5_OFFSET 632
#define ORX_SLOT_O5_SAVE_OFFSET  640
#define ORX_SLOT_CHILD_O6_OFFSET 648
#define ORX_SLOT_O6_SAVE_OFFSET  656
#define ORX_SLOT_CHILD_O7_OFFSET 664
#define ORX_SLOT_O7_SAVE_OFFSET  672

/* Phase 51: per-spawn override of the child's terminal index. The
 * supervisor sets this before each orx_spawn (to its own procid for
 * locally-originated spawns, or the requester's terminal_idx for
 * relayed pass-through spawns). orx_task_create reads it, encodes
 * as `idx + 1` (with -1 → 0 = "no terminal info"), and stuffs into
 * R5 just before TaskCreate. The simulator copies caller's R5 →
 * child's init_r4; crt0.s saves it to _orisc_init_r4; libc's
 * task_init decodes back to my_terminal_idx.
 *
 * The override is consulted only when set (>= 0). When still at the
 * sentinel default (-1), orx_task_create propagates the parent's
 * own terminal_idx — i.e., a non-supervisor caller (a shell that
 * orx_run's a child directly without going through sup_spawn)
 * inherits its terminal naturally. */
static int orx_child_term_override = -1;

void
orx_set_child_terminal_idx(int idx)
{
	orx_child_term_override = idx;
}

void
orx_clear_child_terminal_idx(void)
{
	orx_child_term_override = -1;
}

/* TaskCreate(O1=code, O2=stack, O3=data, O4=args, R4=entry, R5=0)
 * — leave the resulting task ref in O1 for the caller to register
 * into the libc task table via task_register_o1. Restore O2/O3/O4
 * before returning so the caller's print_str / hf_* keep working.
 * Returns firmware status.
 *
 * Phase 45a: if ORX_SLOT_CHILD_O8 is non-null, swap that ref into
 * O8 just before TaskCreate (so the child's task_init harvests
 * it via the boot-O8 → SUP_SLOT path) and restore the parent's
 * O8 after. The override is set by the supervisor so its
 * spawned tasks inherit a working supervisor sub-cap. */
static int
orx_task_create(unsigned int entry, int has_data)
{
	int status;
	int o8_isn, o5_isn, o6_isn, o7_isn;

	/* Probe ORX_SLOT_CHILD_O8 — non-null means we need the
	 * O8-swap dance around TaskCreate. */
	asm volatile(
		"orefld o14, 560(o12)\n"
		"oisn   %0, o14"
		: "=r"(o8_isn)
		:
		: "r1"
	);
	if (!o8_isn) {
		/* Non-null override. Save current O8 to ORX_SLOT_O8_SAVE,
		 * then move the override (just OREFLD'd to O14) into O8. */
		asm volatile(
			"orefst o8, 568(o12)\n"
			"omov   o8, o14"
		);
	}

	/* Phase 49: terminal-pass-through. Mirror the O8 swap dance for
	 * O5/O6/O7 (console/keyboard/grid). The supervisor populates
	 * these slots when a relayed spawn carries a foreign terminal
	 * hint; for local-origin spawns the slots stay null and we skip.
	 *
	 * We probe each independently because a relay may resolve only
	 * a subset (e.g., terminal has console+keyboard but no grid).
	 * Probing via O14 is safe — orx_setup_args already clobbered
	 * O14 above and we re-load it inside each probe block. */
	asm volatile(
		"orefld o14, 632(o12)\n"      /* ORX_SLOT_CHILD_O5 */
		"oisn   %0, o14"
		: "=r"(o5_isn) :: "r1"
	);
	if (!o5_isn) {
		asm volatile(
			"orefst o5, 640(o12)\n"   /* save parent's O5 */
			"omov   o5, o14"          /* O5 = override */
		);
	}
	asm volatile(
		"orefld o14, 648(o12)\n"      /* ORX_SLOT_CHILD_O6 */
		"oisn   %0, o14"
		: "=r"(o6_isn) :: "r1"
	);
	if (!o6_isn) {
		asm volatile(
			"orefst o6, 656(o12)\n"
			"omov   o6, o14"
		);
	}
	asm volatile(
		"orefld o14, 664(o12)\n"      /* ORX_SLOT_CHILD_O7 */
		"oisn   %0, o14"
		: "=r"(o7_isn) :: "r1"
	);
	if (!o7_isn) {
		asm volatile(
			"orefst o7, 672(o12)\n"
			"omov   o7, o14"
		);
	}

	/* Pull the shared argv ref orx_setup_args wrote into
	 * ORX_SLOT_ARGV up to O4. The firmware's TaskCreate reads
	 * O4 as the optional argv buffer, mapped R-only at ARGV_VA
	 * in the child's address space. Done as its own asm so the
	 * main asm body stays short — pcc has been observed to lose
	 * a `\n` inside very long inline asm strings. */
	asm volatile("orefld o4, 536(o12)");
	/* Phase 51: pack the child's terminal_idx + 1 into R5 — the
	 * simulator copies caller's R5 to child's init_r4, crt0 stashes
	 * to _orisc_init_r4, libc task_init reads back. If the supervisor
	 * (or a higher caller) didn't set an override via
	 * orx_set_child_terminal_idx, fall through to the parent's own
	 * terminal_idx so children of a shell inherit naturally. r5_val
	 * 0 = "no terminal info." */
	int term_for_child = (orx_child_term_override >= 0)
	                        ? orx_child_term_override
	                        : task_my_terminal_idx();
	int r5_val = (term_for_child >= 0) ? (term_for_child + 1) : 0;
	if (has_data) {
		asm volatile(
			"orefld o1, 128(o12)\n"
			"orefld o2, 144(o12)\n"
			"orefld o3, 136(o12)\n"
			"addu  r4, %1, r0\n"
			"addu  r5, %2, r0\n"
			"call  #0x000\n"
			"nop\n"
			"omov  o2, o11\n"
			"omov  o3, o15\n"
			"addu  %0, r2, r0"
			: "=r"(status)
			: "r"(entry), "r"(r5_val)
			: "r2", "r3", "r4", "r5"
		);
	} else {
		asm volatile(
			"orefld o1, 128(o12)\n"
			"orefld o2, 144(o12)\n"
			"onull  o3\n"
			"addu  r4, %1, r0\n"
			"addu  r5, %2, r0\n"
			"call  #0x000\n"
			"nop\n"
			"omov  o2, o11\n"
			"omov  o3, o15\n"
			"addu  %0, r2, r0"
			: "=r"(status)
			: "r"(entry), "r"(r5_val)
			: "r2", "r3", "r4", "r5"
		);
	}
	/* Restore parent's O4 to the boot self-svc — keeps the OR-
	 * hygiene contract for callers that rely on O4 between this
	 * return and the next term_print/hf_*. */
	asm volatile("omov o4, o14");

	/* Phase 45a: if we swapped O8 around TaskCreate, restore
	 * the parent's O8 from ORX_SLOT_O8_SAVE. */
	if (!o8_isn) {
		asm volatile("orefld o8, 568(o12)");
	}
	/* Phase 49: same for O5/O6/O7 if we swapped them. */
	if (!o5_isn) {
		asm volatile("orefld o5, 640(o12)");
	}
	if (!o6_isn) {
		asm volatile("orefld o6, 656(o12)");
	}
	if (!o7_isn) {
		asm volatile("orefld o7, 672(o12)");
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
orx_spawn(const char *path, const char *args, const char *cwd)
{
	char hdr[32];

	if (orx_state_init() != 0)
		return -4;

	int fd = vfs_open(path, HF_O_RDONLY);
	if (fd < 0) { return -1; }

	if (vfs_read(fd, hdr, 32) != 32) {
		vfs_close(fd); return -2;
	}
	if (memcmp(hdr, "ORISC\x00\x00\x00", 8) != 0) {
		vfs_close(fd); return -2;
	}
	unsigned int version    = beu32(hdr + 0x08);
	unsigned int entry      = beu32(hdr + 0x10);
	unsigned int text_size  = beu32(hdr + 0x14);
	unsigned int data_size  = beu32(hdr + 0x18);
	unsigned int stack_size = beu32(hdr + 0x1C);
	(void)version;
	if (version != 1 || (text_size & 3) != 0 || entry >= text_size) {
		vfs_close(fd); return -3;
	}
	if (stack_size == 0)
		stack_size = DEFAULT_STACK_SIZE;

	unsigned int code_alloc = round4(text_size);
	unsigned int data_alloc = round4(data_size);
	int has_data = (data_size > 0);

	if (orx_alloc_into_slot(code_alloc, TAG_CODE,
				CAP_R | CAP_W | CAP_X | CAP_V | CAP_C,
				SLOT_CODE) != 0) {
		vfs_close(fd); return -4;
	}
	if (orx_map_slot(SLOT_CODE, TEMP_CODE_VA, code_alloc) != 0) {
		orx_free_slot(SLOT_CODE);
		vfs_close(fd); return -4;
	}
	if (orx_read_into_va(fd, TEMP_CODE_VA, text_size) != 0) {
		orx_unmap(TEMP_CODE_VA, code_alloc);
		orx_free_slot(SLOT_CODE);
		vfs_close(fd); return -4;
	}
	orx_unmap(TEMP_CODE_VA, code_alloc);

	if (has_data) {
		if (orx_alloc_into_slot(data_alloc, TAG_DATA,
					CAP_R | CAP_W | CAP_V | CAP_C,
					SLOT_DATA) != 0) {
			orx_free_slot(SLOT_CODE);
			vfs_close(fd); return -4;
		}
		if (orx_map_slot(SLOT_DATA, TEMP_DATA_VA, data_alloc) != 0) {
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			vfs_close(fd); return -4;
		}
		if (orx_read_into_va(fd, TEMP_DATA_VA, data_size) != 0) {
			orx_unmap(TEMP_DATA_VA, data_alloc);
			orx_free_slot(SLOT_DATA); orx_free_slot(SLOT_CODE);
			vfs_close(fd); return -4;
		}
		orx_unmap(TEMP_DATA_VA, data_alloc);
	}
	vfs_close(fd);

	if (orx_alloc_into_slot(stack_size, TAG_STACK,
				CAP_R | CAP_W | CAP_V | CAP_C,
				SLOT_STACK) != 0) {
		if (has_data) orx_free_slot(SLOT_DATA);
		orx_free_slot(SLOT_CODE);
		return -4;
	}

	if (orx_setup_args(args, cwd) != 0) {
		orx_free_slot(SLOT_STACK);
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
			"orefld o1, 128(o12)\n"
			"orefld o2, 136(o12)\n"
			"orefld o3, 144(o12)"
		);
	} else {
		asm volatile(
			"orefld o1, 128(o12)\n"
			"onull  o2\n"
			"orefld o3, 144(o12)"
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
	/* The manifest lives in O12 (allocated by task_init). If the
	 * caller skipped task_init the OREFLD below would fault — but
	 * any program reaching orx_unload necessarily went through
	 * orx_run, which needs hf_init, which the shell pairs with
	 * task_init. Trusting that. */
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
orx_run(const char *path, const char *args, const char *cwd)
{
	task_t t = orx_spawn(path, args, cwd);
	if (t < 0)
		return t;
	return orx_unload(t);
}

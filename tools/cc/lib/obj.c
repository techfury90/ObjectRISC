/*
 * obj.c — handle-based object/capability API (see obj.h).
 *
 * Capabilities live in a 16-slot table at byte offset OBJ_TABLE_OFFSET
 * of the O12 task-table OBJSTORE (reserved by task.c's ORX_STATE_BYTES).
 * A program holds opaque `obj_t` handles; libc OREFLDs the capability
 * into O1/O2 for each operation and OREFSTs new ones back. Because the
 * capability never becomes a C `void *__or` value, none of this trips
 * the v1 backend's "can't hold an OR across a call" limit.
 *
 * OREFLD/OREFST take only an immediate offset, so the slot<->O-reg moves
 * switch on the (compile-time) per-slot offsets — the orx.c idiom. The
 * load/store helpers leave / take the capability in O1 (O2 for the
 * second obj_eq operand); each caller's firmware trap or OR instruction
 * uses that register immediately, before anything can clobber it.
 */

#include "liborisc.h"
#include "obj.h"

/* The hard-coded per-slot offsets below assume this base; catch drift. */
typedef char obj__off_check[(OBJ_TABLE_OFFSET == 1704) ? 1 : -1];

/* bit h set => handle h is allocated. */
static unsigned int obj_inuse;

/* Set once obj_init has run, so a second caller (another migrated
 * subsystem ensuring the table is up) doesn't zero out live handles. */
static int obj_inited;

/* --- slot <-> O-register moves (immediate-offset switch) ------------- *
 * Defined in objslot.c, split out so growing the handle table to 16
 * slots does not bloat obj.c past the pcc label-number point where a
 * backend asm-emission bug corrupts obj_send's multi-line SEND template
 * (an inter-line newline emitted as a literal n). Declared here; the
 * "OR survives the call" discipline holds across the TU boundary (pcc
 * never models an O-register as a value, so the register each leaves set
 * or reads is intact across the call). */
void obj__load_o1(obj_t h);
void obj__load_o2(obj_t h);
void obj__load_o3(obj_t h);
void obj__load_o4(obj_t h);
void obj__store_o1(obj_t h);
void obj__store_o2(obj_t h);

static int
obj__alloc_handle(void)
{
	int h;

	for (h = 0; h < OBJ_NHANDLE; h++)
		if ((obj_inuse & (1u << h)) == 0)
			return h;
	return OBJ_NULL;
}

/* --- init ----------------------------------------------------------- */

int
obj_init(void)
{
	int o12_null;

	/* O12 is the task table; null means task_init hasn't run. */
	asm volatile("oisn %0, o12" : "=r"(o12_null));
	if (o12_null)
		return -1;
	if (!obj_inited) {             /* idempotent — don't clobber live handles */
		obj_inuse = 0;
		obj_inited = 1;
	}
	return 0;
}

/* --- lifecycle ------------------------------------------------------ */

obj_t
obj_alloc(unsigned int len, unsigned int tag, unsigned int caps)
{
	int h, status;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	/* addus reverse-ordered: pcc may have placed %1/%2/%3 in r4/r5/r6
	 * already, so consume the highest first (orx.c idiom). */
	asm volatile(
		"addu r6, %3, r0\n"
		"addu r5, %2, r0\n"
		"addu r4, %1, r0\n"
		"call #0x100\n"           /* ObjAlloc -> O1 = ref, R2 = status */
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(len), "r"(tag), "r"(caps)
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status != 0)
		return OBJ_NULL;
	obj__store_o1(h);              /* O1 still holds the fresh ref */
	obj_inuse |= (1u << h);
	return h;
}

obj_t
obj_alloc_store(unsigned int len, unsigned int tag, unsigned int caps)
{
	int h, status;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	asm volatile(
		"addu r6, %3, r0\n"
		"addu r5, %2, r0\n"
		"addu r4, %1, r0\n"
		"call #0x106\n"           /* ObjAllocStore -> O1 = ref, R2 = status */
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(len), "r"(tag), "r"(caps)
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status != 0)
		return OBJ_NULL;
	obj__store_o1(h);
	obj_inuse |= (1u << h);
	return h;
}

obj_t
obj_derive(obj_t src, unsigned int caps)
{
	int h, status;

	if (src < 0 || src >= OBJ_NHANDLE || (obj_inuse & (1u << src)) == 0)
		return OBJ_NULL;
	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	obj__load_o1(src);             /* O1 = source capability */
	asm volatile(
		"addu r4, %1, r0\n"
		"call #0x103\n"           /* ObjDerive: O1, R4=mask -> O1, R2 */
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(caps)
		: "r2", "r3", "r4"
	);
	if (status != 0)
		return OBJ_NULL;
	obj__store_o1(h);              /* O1 now holds the derived ref */
	obj_inuse |= (1u << h);
	return h;
}

int
obj_free(obj_t h)
{
	int status;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);
	asm volatile(
		"call #0x101\n"           /* ObjFree: O1 -> R2 = status */
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		:
		: "r2", "r3"
	);
	obj_inuse &= ~(1u << h);
	return status;
}

void
obj_drop(obj_t h)
{
	/* Just release the handle; the object is owned elsewhere. The stale
	 * ref left in the slot is unreachable (the in-use bit gates every
	 * access) and is overwritten when the slot is next allocated. */
	if (h < 0 || h >= OBJ_NHANDLE)
		return;
	obj_inuse &= ~(1u << h);
}

/* DIR_RESULT slot in the O12 task table — where dir_walk publishes its
 * resolved ref (task.c's slot map). Hard offset because OREFLD takes an
 * immediate; drift shows up as a wrong/null cap, caught by tests. */
#define OBJ_DIR_RESULT_OFFSET 616

obj_t
obj_adopt_dir_result(void)
{
	int h, isn;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	asm volatile(
		"orefld o1, %1(o12)\n"    /* O1 = dir_walk's resolved ref */
		"oisn   %0, o1"
		: "=r"(isn)
		: "i"(OBJ_DIR_RESULT_OFFSET)
		: "r1"
	);
	if (isn)
		return OBJ_NULL;          /* dir result was null — nothing to adopt */
	obj__store_o1(h);             /* O1 still holds the ref (orx.c idiom) */
	obj_inuse |= (1u << h);
	return h;
}

/* Adopt the capability currently in a known libc O12 slot into a handle.
 * A migration bridge for the directory clients (dir.c / sup.c): their
 * service / parent / reply / input-ref caps are bootstrapped into raw
 * O12 slots by other code (task_init, supervisor boot, the caller's O1
 * at entry), and this brings one into the handle world so obj_send_3or /
 * obj_recv_* can use it. `off` must be one of the slot offsets below
 * (OREFLD takes an immediate). OBJ_NULL if the slot is null or the table
 * is full. */
obj_t
obj_adopt_slot(int off)
{
	int h, isn;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	switch (off) {
	case 544: asm volatile("orefld o1, 544(o12)\n oisn %0, o1" : "=r"(isn) : : "r1"); break;
	case 552: asm volatile("orefld o1, 552(o12)\n oisn %0, o1" : "=r"(isn) : : "r1"); break;
	case 584: asm volatile("orefld o1, 584(o12)\n oisn %0, o1" : "=r"(isn) : : "r1"); break;
	case 624: asm volatile("orefld o1, 624(o12)\n oisn %0, o1" : "=r"(isn) : : "r1"); break;
	default:  return OBJ_NULL;
	}
	if (isn)
		return OBJ_NULL;          /* slot was null — nothing to adopt */
	obj__store_o1(h);             /* O1 still holds the ref (orx.c idiom) */
	obj_inuse |= (1u << h);
	return h;
}

/* Publish handle `h`'s capability back into the DIR_RESULT slot (616) —
 * the inverse of obj_adopt_dir_result. A compatibility bridge for the
 * legacy direct-616 consumers: wm_bind_surface receives its resolved
 * surface cap with obj_recv_cap (into a handle) but still mirrors it
 * here, because the already-migrated graphics clients
 * (vec/raster/pointer_init_from_dir_result) and wm_open_session read the
 * cap straight from 616. Drops away once those consumers take the handle
 * directly. */
void
obj_park_dir_result(obj_t h)
{
	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return;
	obj__load_o1(h);              /* O1 = handle's capability */
	asm volatile("orefst o1, %0(o12)" : : "i"(OBJ_DIR_RESULT_OFFSET) : "r1");
}

obj_t
obj_adopt_o6(void)
{
	int h, isn;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	asm volatile(
		"omov o1, o6\n"           /* O1 = boot keyboard-service cap */
		"oisn %0, o1"
		: "=r"(isn)
		:
		: "r1"
	);
	if (isn)
		return OBJ_NULL;          /* O6 null — nothing to adopt */
	obj__store_o1(h);
	obj_inuse |= (1u << h);
	return h;
}

obj_t
obj_adopt_o7(void)
{
	int h, isn;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	asm volatile(
		"omov o1, o7\n"           /* O1 = boot grid-service cap */
		"oisn %0, o1"
		: "=r"(isn)
		:
		: "r1"
	);
	if (isn)
		return OBJ_NULL;          /* O7 null — nothing to adopt */
	obj__store_o1(h);
	obj_inuse |= (1u << h);
	return h;
}

obj_t
obj_adopt_o10(void)
{
	int h, isn;

	h = obj__alloc_handle();
	if (h < 0)
		return OBJ_NULL;
	asm volatile(
		"omov o1, o10\n"          /* O1 = boot hostfsd-service cap */
		"oisn %0, o1"
		: "=r"(isn)
		:
		: "r1"
	);
	if (isn)
		return OBJ_NULL;          /* O10 null — nothing to adopt */
	obj__store_o1(h);
	obj_inuse |= (1u << h);
	return h;
}

void
obj_park_o8(obj_t h)
{
	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return;
	obj__load_o1(h);              /* O1 = handle's capability */
	asm volatile("omov o8, o1");  /* O8 = compat mirror (see obj.h) */
}

/* --- inspection ----------------------------------------------------- */

int
obj_isnull(obj_t h)
{
	int r;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return 1;
	obj__load_o1(h);
	asm volatile("oisn %0, o1" : "=r"(r));
	return r;
}

int
obj_eq(obj_t a, obj_t b)
{
	int r;

	if (a < 0 || a >= OBJ_NHANDLE || b < 0 || b >= OBJ_NHANDLE)
		return 0;
	obj__load_o1(a);
	obj__load_o2(b);               /* leaves O1 (a) intact */
	asm volatile("oeq %0, o1, o2" : "=r"(r));
	return r;
}

int
obj_len(obj_t h)
{
	int r;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);
	asm volatile("olen %0, o1" : "=r"(r));
	return r;
}

int
obj_tag(obj_t h)
{
	int r;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);
	asm volatile("otag %0, o1" : "=r"(r));
	return r;
}

int
obj_caps(obj_t h)
{
	int r;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);
	asm volatile("ocap %0, o1" : "=r"(r));
	return r;
}

/* --- byte access at offset 0 ---------------------------------------- */

int
obj_loadw(obj_t h)
{
	int r;

	obj__load_o1(h);
	asm volatile("olw %0, 0(o1)\n nop" : "=r"(r));
	return r;
}

void
obj_storew(obj_t h, int val)
{
	obj__load_o1(h);
	asm volatile("osw %0, 0(o1)" : : "r"(val));
}

/* --- messaging ------------------------------------------------------ */

int
obj_queue_attach(obj_t h, unsigned int depth)
{
	int status;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* queue object -> O1 */
	asm volatile(
		"addu r4, %1, r0\n"
		"call #0x203\n"           /* ReceiveQueueAttach: O1, R4=depth -> R2 */
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(depth)
		: "r2", "r3", "r4"
	);
	return status;
}

int
obj_send(obj_t h, int a0, int a1, int a2, int a3)
{
	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* recipient -> O1 */
	asm volatile(
		"onull o2\n onull o3\n onull o4\n"
		"addu r7, %3, r0\n"
		"addu r6, %2, r0\n"
		"addu r5, %1, r0\n"
		"addu r4, %0, r0\n"
		"send o1"
		:
		: "r"(a0), "r"(a1), "r"(a2), "r"(a3)
		: "r4", "r5", "r6", "r7", "o2", "o3", "o4"
	);
	return 0;                       /* SEND traps on error rather than status */
}

int
obj_send_or(obj_t h, obj_t or_h, int a0, int a1, int a2, int a3)
{
	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* recipient -> O1 */
	if (or_h >= 0 && or_h < OBJ_NHANDLE && (obj_inuse & (1u << or_h)))
		obj__load_o2(or_h);    /* payload capability -> O2 (leaves O1) */
	else
		asm volatile("onull o2");   /* OBJ_NULL -> null O2 (unsubscribe) */
	asm volatile(
		"onull o3\n onull o4\n"
		"addu r7, %3, r0\n"
		"addu r6, %2, r0\n"
		"addu r5, %1, r0\n"
		"addu r4, %0, r0\n"
		"send o1"
		:
		: "r"(a0), "r"(a1), "r"(a2), "r"(a3)
		: "r4", "r5", "r6", "r7", "o3", "o4"
	);
	return 0;
}

int
obj_send_bytes(obj_t svc, int src, obj_t reply,
               int a0, int a1, int a2, int a3)
{
	if (svc < 0 || svc >= OBJ_NHANDLE || (obj_inuse & (1u << svc)) == 0)
		return -1;
	obj__load_o1(svc);             /* service -> O1 */
	if (src == OBJ_SRC_STACK)
		asm volatile("omov o2, o11");   /* boot stack ref -> O2 */
	else if (src == OBJ_SRC_DATA)
		asm volatile("omov o2, o15");   /* boot data ref -> O2 */
	else
		asm volatile("onull o2");       /* OBJ_SRC_NONE */
	if (reply >= 0 && reply < OBJ_NHANDLE && (obj_inuse & (1u << reply)))
		obj__load_o3(reply);   /* reply-cap -> O3 (leaves O1/O2) */
	else
		asm volatile("onull o3");
	/* O1/O2/O3 are set; the send reads them. Only O4 + R4..R7 here. */
	asm volatile(
		"onull o4\n"
		"addu r7, %3, r0\n"
		"addu r6, %2, r0\n"
		"addu r5, %1, r0\n"
		"addu r4, %0, r0\n"
		"send o1"
		:
		: "r"(a0), "r"(a1), "r"(a2), "r"(a3)
		: "r4", "r5", "r6", "r7", "o4"
	);
	return 0;
}

int
obj_send_3or(obj_t svc, obj_t or2, obj_t or3, obj_t or4,
             int a0, int a1, int a2, int a3)
{
	if (svc < 0 || svc >= OBJ_NHANDLE || (obj_inuse & (1u << svc)) == 0)
		return -1;
	/* Load each OR payload from its handle slot (OREFLD writes only its
	 * target reg, so the three loads don't disturb each other), or null
	 * the slot for an OBJ_NULL handle. Same "the compiler never models an
	 * OR as a value, so nothing spills it across the helper calls"
	 * discipline obj_send_bytes relies on — extended to O4. */
	obj__load_o1(svc);                 /* recipient -> O1 */
	if (or2 >= 0 && or2 < OBJ_NHANDLE && (obj_inuse & (1u << or2)))
		obj__load_o2(or2);
	else
		asm volatile("onull o2");
	if (or3 >= 0 && or3 < OBJ_NHANDLE && (obj_inuse & (1u << or3)))
		obj__load_o3(or3);
	else
		asm volatile("onull o3");
	if (or4 >= 0 && or4 < OBJ_NHANDLE && (obj_inuse & (1u << or4)))
		obj__load_o4(or4);
	else
		asm volatile("onull o4");
	asm volatile(
		"addu r7, %3, r0\n"
		"addu r6, %2, r0\n"
		"addu r5, %1, r0\n"
		"addu r4, %0, r0\n"
		"send o1"
		:
		: "r"(a0), "r"(a1), "r"(a2), "r"(a3)
		: "r4", "r5", "r6", "r7"
	);
	return 0;
}

/* Fixed staging-buffer size for obj_make_bytes. The object is always
 * this many bytes; the service reads only the first `len`. A fixed size
 * lets us pass it as an asm *immediate* — crucial, because pcc-orisc
 * corrupts a size held in a C variable across the obj_alloc call (it
 * reloads as <=0, so MapObject returns EINVAL). dir.c hit the same wall
 * and uses a fixed DIR_PATH_BUF_SIZE the same way. `len` must be
 * <= OBJ_BYTES_MAX. The staging VA is 0x600000 (above the 0x500000 argv
 * mapping, matching dir.c/sup.c). */
#define OBJ_BYTES_MAX  256

obj_t
obj_make_bytes(const char *src, int len, unsigned int caps)
{
	obj_t h;
	int status, i;
	char *dst;

	if (len <= 0 || len > OBJ_BYTES_MAX)
		return OBJ_NULL;
	h = obj_alloc(OBJ_BYTES_MAX, OBJ_TAG_DATA, caps);
	if (h < 0)
		return OBJ_NULL;

	obj__load_o1(h);                   /* O1 = the fresh bytes object */
	asm volatile(                      /* MapObject(O1, VA, off=0, R|W, MAX) */
		"lui   r4, 0x60\n"
		"addu  r5, r0, r0\n"
		"addiu r6, r0, %1\n"
		"addiu r7, r0, %2\n"           /* size as an IMMEDIATE, not a var */
		"call  #0x110\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(OBJ_CAP_R | OBJ_CAP_W), "i"(OBJ_BYTES_MAX)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) { obj_free(h); return OBJ_NULL; }

	/* pcc rejects (char *)0x600000 as a literal cast; synthesize the VA
	 * via lui+ori, exactly as dir.c does. */
	asm volatile("lui %0, 0x60\n ori %0, %0, 0" : "=r"(dst));
	for (i = 0; i < len && i < OBJ_BYTES_MAX; i++)
		dst[i] = src[i];

	asm volatile(                      /* Unmap(VA, MAX) */
		"lui   r4, 0x60\n"
		"addiu r5, r0, %1\n"
		"call  #0x111\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(OBJ_BYTES_MAX)
		: "r2", "r3", "r4", "r5"
	);
	if (status != 0) { obj_free(h); return OBJ_NULL; }
	return h;
}

int
obj_recv(obj_t h)
{
	int status, word;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* queue object -> O1 */
	asm volatile(
		"addiu r4, r0, -1\n"      /* infinite wait */
		"call #0x204\n"           /* ReceiveQueuePoll -> R2 status, R3 word */
		"nop\n"
		"addu %0, r2, r0\n"
		"addu %1, r3, r0"
		: "=r"(status), "=r"(word)
		:
		: "r2", "r3", "r4", "r5", "r6"
	);
	if (status != 0)
		return -1;
	return word;
}

/* Status-via-global: ReceiveQueuePoll yields 5 results (status + 4
 * payload words) but pcc allows only 4 asm outputs, so R2 (status) is
 * sw'd to this global from inside the asm body — the pointer_getevent /
 * poll_window_grids idiom. */
static int obj__poll_status;

int
obj_poll(obj_t h, int out[4])
{
	int w0, w1, w2, w3;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* queue object -> O1 */
	asm volatile(
		"addiu r4, r0, 0\n"       /* timeout 0 = non-blocking poll */
		"call  #0x204\n"          /* ReceiveQueuePoll -> R2 status, R3..R6 */
		"nop\n"
		"la    r1, obj__poll_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(w0), "=r"(w1), "=r"(w2), "=r"(w3)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	if (obj__poll_status != 0)
		return -1;
	out[0] = w0;
	out[1] = w1;
	out[2] = w2;
	out[3] = w3;
	return 0;
}

int
obj_recv_full(obj_t h, int out[4])
{
	int w0, w1, w2, w3;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	obj__load_o1(h);               /* queue object -> O1 */
	asm volatile(
		"addiu r4, r0, -1\n"      /* timeout -1 = block until a message */
		"call  #0x204\n"          /* ReceiveQueuePoll -> R2 status, R3..R6 */
		"nop\n"
		"la    r1, obj__poll_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(w0), "=r"(w1), "=r"(w2), "=r"(w3)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	if (obj__poll_status != 0)
		return -1;
	out[0] = w0;
	out[1] = w1;
	out[2] = w2;
	out[3] = w3;
	return 0;
}

int
obj_recv_cap(obj_t h, int *out_word)
{
	int hh, status, word3, isn;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return OBJ_NULL;
	hh = obj__alloc_handle();
	if (hh < 0)
		return OBJ_NULL;
	obj__load_o1(h);               /* queue object -> O1 */
	asm volatile(
		"addiu r4, r0, -1\n"      /* block until a message */
		"call  #0x204\n"          /* ReceiveQueuePoll -> R2 status, R3.., O2 = reply cap */
		"nop\n"
		"oisn  %2, o2\n"          /* is the reply's O2 capability null? */
		"addu  %0, r2, r0\n"      /* poll status */
		"addu  %1, r3, r0"        /* R3 reply word */
		: "=r"(status), "=r"(word3), "=r"(isn)
		:
		: "r2", "r3", "r4", "r5", "r6"
	);
	/* Land the reply cap into the handle slot BEFORE any C that might
	 * touch O2 — the same "O1 survives the call" discipline obj_alloc
	 * relies on for obj__store_o1. On the error paths the slot holds a
	 * stale ref but stays un-in-use, so it is never read. */
	obj__store_o2(hh);
	if (out_word) *out_word = word3;
	if (status != 0 || isn)
		return OBJ_NULL;          /* poll failed, or no cap in the reply */
	obj_inuse |= (1u << hh);
	return hh;
}

obj_t
obj_recv_cap_full(obj_t h, int out[4])
{
	int hh, w0, w1, w2, w3;

	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return OBJ_NULL;
	hh = obj__alloc_handle();
	if (hh < 0)
		return OBJ_NULL;
	obj__load_o1(h);               /* queue object -> O1 */
	/* Four asm outputs is the ceiling, so the poll STATUS (R2) is sw'd to
	 * obj__poll_status (the obj_recv_full idiom), leaving R3..R6 for
	 * out[]; the O2 cap is OREFST'd into the handle slot right after. */
	asm volatile(
		"addiu r4, r0, -1\n"      /* block until a message */
		"call  #0x204\n"          /* ReceiveQueuePoll -> R2 st, R3..R6, O2 = cap */
		"nop\n"
		"la    r1, obj__poll_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(w0), "=r"(w1), "=r"(w2), "=r"(w3)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	/* Land the cap (still in O2) into the slot before any C touches O2.
	 * Unlike obj_recv_cap, a NULL reply cap is NOT a failure here — a
	 * dir_walk on a plain directory resolves no ref, yet R3..R6 are still
	 * valid. So we keep the handle on any successful poll (the caller
	 * checks obj_isnull / parks it); only a poll error returns OBJ_NULL. */
	obj__store_o2(hh);
	out[0] = w0; out[1] = w1; out[2] = w2; out[3] = w3;
	if (obj__poll_status != 0)
		return OBJ_NULL;          /* poll itself failed */
	obj_inuse |= (1u << hh);
	return hh;
}

int
obj_fetch_to_stack(obj_t src, int dst_off, int count)
{
	int status;

	if (src < 0 || src >= OBJ_NHANDLE || (obj_inuse & (1u << src)) == 0)
		return -1;
	obj__load_o1(src);             /* O1 = source object */
	asm volatile(
		"omov  o2, o11\n"         /* O2 = boot stack (destination) */
		"addiu r4, r0, 0\n"       /* src offset 0 */
		"addu  r5, %1, r0\n"      /* dst offset within the stack */
		"addu  r6, %2, r0\n"      /* byte count */
		"call  #0x108\n"          /* ObjFetchBytes -> R2 = status */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "r"(dst_off), "r"(count)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

task_t
obj_register_task(obj_t h)
{
	if (h < 0 || h >= OBJ_NHANDLE || (obj_inuse & (1u << h)) == 0)
		return -1;
	/* Load the task ref into O1 and hand off to task.c's slot-table
	 * enroller, which OREFSTs O1 into the next free task slot. O1 is set
	 * by obj__load_o1's asm (not modelled by pcc as a value), so it
	 * survives the immediately-following task_register_o1 call unspilled —
	 * the same "OR survives the call" discipline obj_alloc/obj_derive rely
	 * on, and exactly what sup_spawn's hand-written poll asm used to do
	 * with `omov o1, o2; return task_register_o1();`. */
	obj__load_o1(h);
	return task_register_o1();
}

/*
 * obj.c — handle-based object/capability API (see obj.h).
 *
 * Capabilities live in an 8-slot table at byte offset OBJ_TABLE_OFFSET
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

/* --- slot <-> O-register moves (immediate-offset switch) ------------- */

static void
obj__load_o1(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o1, 1704(o12)"); break;
	case 1: asm volatile("orefld o1, 1712(o12)"); break;
	case 2: asm volatile("orefld o1, 1720(o12)"); break;
	case 3: asm volatile("orefld o1, 1728(o12)"); break;
	case 4: asm volatile("orefld o1, 1736(o12)"); break;
	case 5: asm volatile("orefld o1, 1744(o12)"); break;
	case 6: asm volatile("orefld o1, 1752(o12)"); break;
	case 7: asm volatile("orefld o1, 1760(o12)"); break;
	}
}

static void
obj__load_o2(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o2, 1704(o12)"); break;
	case 1: asm volatile("orefld o2, 1712(o12)"); break;
	case 2: asm volatile("orefld o2, 1720(o12)"); break;
	case 3: asm volatile("orefld o2, 1728(o12)"); break;
	case 4: asm volatile("orefld o2, 1736(o12)"); break;
	case 5: asm volatile("orefld o2, 1744(o12)"); break;
	case 6: asm volatile("orefld o2, 1752(o12)"); break;
	case 7: asm volatile("orefld o2, 1760(o12)"); break;
	}
}

static void
obj__store_o1(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefst o1, 1704(o12)"); break;
	case 1: asm volatile("orefst o1, 1712(o12)"); break;
	case 2: asm volatile("orefst o1, 1720(o12)"); break;
	case 3: asm volatile("orefst o1, 1728(o12)"); break;
	case 4: asm volatile("orefst o1, 1736(o12)"); break;
	case 5: asm volatile("orefst o1, 1744(o12)"); break;
	case 6: asm volatile("orefst o1, 1752(o12)"); break;
	case 7: asm volatile("orefst o1, 1760(o12)"); break;
	}
}

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
	obj_inuse = 0;
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

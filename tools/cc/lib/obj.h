/*
 * obj.h — Object RISC libc: a handle-based object/capability API.
 *
 * Why handles, not `void *__or`: the v1 pcc backend cannot hold an
 * `__or` capability *value* across a call (it would byte-spill the
 * capability, which the architecture forbids — see
 * tools/cc/arch/orisc/OREG_MIGRATION_PLAN.md). So instead of returning
 * capabilities to C, this API keeps them in a small per-task table that
 * libc owns (in the O12 task-table OBJSTORE) and hands the program an
 * opaque integer `obj_t` handle — exactly the file-descriptor pattern
 * host_io.c already uses for hostfsd. Capabilities never appear as C
 * values, so handle-based code compiles and runs today.
 *
 * Lifecycle: call task_init() (sets up O12), then obj_init() once, then
 * obj_alloc/derive/... Every call returns <0 on error (OBJ_NULL for the
 * handle-returning ones).
 *
 * The table has OBJ_NHANDLE slots; obj_alloc/derive/recv fail with
 * OBJ_NULL when all are in use. obj_free releases one.
 */

#ifndef OBJ_H
#define OBJ_H

typedef int obj_t;

#define OBJ_NULL      (-1)
#define OBJ_NHANDLE   8

/* Byte offset of the handle table within the O12 task-table OBJSTORE.
 * Sits just past the compiler OR-spill anchor (1696); task.c reserves
 * OBJ_NHANDLE*8 bytes here by oversizing ORX_STATE_BYTES. The per-slot
 * offsets are hard-coded in obj.c's load/store switches (OREFLD/OREFST
 * take only an immediate offset), guarded by a static check there. */
#define OBJ_TABLE_OFFSET   1704

/* Object type tags (Vol II §3.2). */
#define OBJ_TAG_CODE     0x4100
#define OBJ_TAG_STACK    0x4101
#define OBJ_TAG_DATA     0x4102
#define OBJ_TAG_SERVICE  0x4103

/* Capability bits (Vol III §2). */
#define OBJ_CAP_R  0x01
#define OBJ_CAP_W  0x02
#define OBJ_CAP_X  0x04
#define OBJ_CAP_S  0x08
#define OBJ_CAP_V  0x10
#define OBJ_CAP_C  0x40

/* One-time init (after task_init). Returns 0, or -1 if O12 isn't set up. */
int   obj_init(void);

/* --- lifecycle ------------------------------------------------------ */

/* Allocate a byte-addressable object (len bytes, type `tag`, caps).
 * Returns a handle, or OBJ_NULL on firmware error / table full. */
obj_t obj_alloc(unsigned int len, unsigned int tag, unsigned int caps);

/* Like obj_alloc but the storage is OR-typed (OREFLD/OREFST access it;
 * integer OL/OS trap). `len` must be a multiple of 8. */
obj_t obj_alloc_store(unsigned int len, unsigned int tag, unsigned int caps);

/* Derive a sub-capability of `src` carrying a subset of its caps. */
obj_t obj_derive(obj_t src, unsigned int caps);

/* Free the object and release the handle (the object's ref needs CAP_V).
 * Returns firmware status (0 = freed). */
int   obj_free(obj_t h);

/* Release a borrowed/derived handle WITHOUT freeing its object. An
 * obj_derive result is a sub-reference to the owner's object (same
 * descriptor, fewer caps), so it must be dropped, not freed. */
void  obj_drop(obj_t h);

/* --- inspection (no memory access) ---------------------------------- */

int   obj_isnull(obj_t h);            /* 1 if the slot holds a null ref */
int   obj_eq(obj_t a, obj_t b);        /* 1 if a and b name the same object */
int   obj_len(obj_t h);                /* storage length in bytes */
int   obj_tag(obj_t h);                /* type-tag word */
int   obj_caps(obj_t h);               /* capability bits */

/* --- byte access at offset 0 (handle's object needs R / W caps) ----- */

int   obj_loadw(obj_t h);              /* OLW word at offset 0 */
void  obj_storew(obj_t h, int val);    /* OSW word at offset 0 */

/* --- messaging ------------------------------------------------------ */

/* SEND to the service named by `h` (needs S cap), payload in R4..R7;
 * no OR-register payload. Returns firmware status (0 = sent). */
int   obj_send(obj_t h, int a0, int a1, int a2, int a3);

/* Block on the receive queue attached to `h` until a message arrives;
 * returns its R3 word (or <0 on poll error). Caller reads the rest of
 * the payload via a follow-up obj_recv variant in a later revision. */
int   obj_recv(obj_t h);

#endif /* OBJ_H */

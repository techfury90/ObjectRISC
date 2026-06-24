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

/* Adopt the capability that dir_walk resolved into the per-task
 * DIR_RESULT slot (its LEAF target / MOUNT service ref) as a handle —
 * the clean way to bring a directory-resolved service cap into the
 * handle world without the asm holding it in an O-register across a
 * call. Returns OBJ_NULL if that slot is null or the table is full. */
obj_t obj_adopt_dir_result(void);

/* Adopt the capability in boot register O6 (the keyboard service, per
 * liborisc's boot map) as a handle. Reads O6 inside libc so the cap
 * never crosses a call boundary in an O-register. OBJ_NULL if O6 is
 * null or the table is full. */
obj_t obj_adopt_o6(void);

/* Adopt the capability in boot register O7 (the grid / positioned-text
 * service, per liborisc's boot map) as a handle. Same read-inside-libc
 * discipline as obj_adopt_o6. OBJ_NULL if O7 is null or the table is
 * full. */
obj_t obj_adopt_o7(void);

/* Adopt the capability in boot register O10 (the hostfsd service, per
 * liborisc's boot map for hostfs-using programs) as a handle. Same
 * read-inside-libc discipline as obj_adopt_o6. OBJ_NULL if O10 is null
 * or the table is full. */
obj_t obj_adopt_o10(void);

/* Park handle `h`'s capability into boot register O8 — a compatibility
 * mirror for the legacy direct-O8 consumers. host_io's hf mailbox now
 * lives canonically in the handle table, but term.c's term_print_n_sync
 * still derives its reply-cap from O8 / blocks on O8, and the supervisor
 * harvests a child's O8 around TaskCreate, so hf_init mirrors the
 * mailbox cap here. Drop the mirror once those consumers migrate too. */
void  obj_park_o8(obj_t h);

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

/* Attach a receive queue (`depth` slots) to the handle's object so it
 * can receive SENDs — required for a service mailbox before subscribers
 * or senders target it. Returns firmware status (0 = attached). */
int   obj_queue_attach(obj_t h, unsigned int depth);

/* SEND to the service named by `h` (needs S cap), payload in R4..R7;
 * no OR-register payload. Returns firmware status (0 = sent). */
int   obj_send(obj_t h, int a0, int a1, int a2, int a3);

/* Like obj_send, but carries an OR-register payload: O2 = `or_h`'s
 * capability, or null O2 when `or_h` is OBJ_NULL (the coarse v1
 * unsubscribe convention). Used to hand a service a sub-cap of your
 * mailbox (subscribe). Returns 0 (SEND traps on error). */
int   obj_send_or(obj_t h, obj_t or_h, int a0, int a1, int a2, int a3);

/* Source segment for obj_send_bytes' O2 — where the payload bytes live
 * so the service can ObjFetchBytes them. */
#define OBJ_SRC_NONE   0       /* no byte payload — null O2 */
#define OBJ_SRC_STACK  1       /* boot stack ref (O11): stack-local buffers */
#define OBJ_SRC_DATA   2       /* boot data ref (O15): static/global buffers */

/* The data-send keystone: SEND a byte-data request to service `svc`.
 * O2 = the `src` segment ref (OBJ_SRC_STACK/DATA, or null for
 * OBJ_SRC_NONE) so the service ObjFetchBytes the payload; O3 = `reply`'s
 * mailbox cap (the reply-cap) or null when reply == OBJ_NULL; R4..R7 =
 * a0..a3 (op + the byte offset/count and any params — the caller
 * computes the offset into the chosen segment). Returns 0 (SEND traps on
 * error), -1 if `svc` is invalid. This is what every message-with-data
 * client (host_io, term console, sup, dir, raster, grid) needs. */
int   obj_send_bytes(obj_t svc, int src, obj_t reply,
                     int a0, int a1, int a2, int a3);

/* Block on the receive queue attached to `h` until a message arrives;
 * returns its R3 word (or <0 on poll error). Caller reads the rest of
 * the payload via a follow-up obj_recv variant in a later revision. */
int   obj_recv(obj_t h);

/* Non-blocking poll of the receive queue attached to `h`: on a message,
 * writes the R3..R6 payload words to out[0..3] and returns 0; returns
 * -1 when the queue is empty or on error. */
int   obj_poll(obj_t h, int out[4]);

/* Blocking sibling of obj_poll: waits for a message on `h`'s queue, then
 * writes the R3..R6 payload words to out[0..3]; returns 0, or -1 on poll
 * error. */
int   obj_recv_full(obj_t h, int out[4]);

#endif /* OBJ_H */

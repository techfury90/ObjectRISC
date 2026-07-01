/*
 * obj_or.h — Object RISC libc: the ergonomic `void *__or` capability-VALUE
 * object API. The sibling of obj.h (the handle API): where obj.h hands you
 * an opaque `obj_t` int indexing a 16-slot per-task table, this API takes
 * and returns capabilities AS C values (`void *__or`) — no handles, no
 * table, no 16-slot ceiling.
 *
 * Why this can exist now: the v1 pcc backend used to be unable to hold an
 * `__or` capability value across a call, so obj.h boxed caps behind
 * handles. The compiler fix (macdefs.h PRUNE_CALLLIVE — clears the spurious
 * cross-call OR liveness) lets a capability live in a C variable and cross
 * a call, homed in the per-frame OBJSTORE (see
 * tools/cc/arch/orisc/OREG_MIGRATION_PLAN.md and examples/cc/oref_*.c). So
 * the natural form finally compiles:
 *
 *     void *__or o = objor_alloc(64, OBJ_TAG_DATA, OBJ_CAP_R|OBJ_CAP_W);
 *     objor_storew(o, 0xABC);
 *     other();                         // a clobbering call
 *     v = objor_loadw(o);              // o still names the object
 *
 * IMPLEMENTATION NOTE — the ops are bare assembly (obj_or.s), NOT C. The
 * C `__or` calling convention already places `__or` args in O1..O3, int
 * args in R4..R7, the `__or` return in O1 and the int return in R2 — which
 * is *exactly* the firmware register ABI for these primitives. So each op
 * is a thin passthrough (`call #0xNNN; jr r31`), the same technique
 * console_io.s uses for orisc_console_write. Writing them in C would drag
 * in the per-frame OBJSTORE prologue, whose ObjAllocStore setup clobbers
 * the incoming R4..R6 int params before they are homed — so a C op that
 * both takes an `__or` param and forwards an int param to a firmware call
 * loses the int. Bare asm sidesteps that entirely.
 *
 * LIFECYCLE / SCOPE RULES:
 *   - Call task_init() before ANY function whose body holds an `__or` auto
 *     or param: that function's prologue allocates a per-frame OBJSTORE and
 *     chains it through the O12 task table, which task_init() sets up. In
 *     practice: task_init() in main(), then delegate the `__or` work to a
 *     helper (main() itself must stay free of `__or` autos — its prologue
 *     runs before its body can call task_init). See examples/cc/oref_*.c.
 *   - obj_init() is NOT needed (there is no handle table to initialise).
 *   - A capability you OWN (allocated, has CAP_V) is released with
 *     objor_free. A sub-reference from objor_derive shares the owner's
 *     descriptor (same gen/home/index, fewer caps) — do NOT objor_free it
 *     (ObjFree needs CAP_V, which a narrowed sub-cap typically lacks →
 *     EPERM). You "drop" a sub-ref simply by letting the C value fall out
 *     of scope; objor_drop() is a no-op provided for intent/symmetry.
 *
 * The OBJ_TAG_* / OBJ_CAP_* constants are shared with the handle API — this
 * header pulls them from obj.h so the two APIs speak the same vocabulary.
 */

#ifndef OBJ_OR_H
#define OBJ_OR_H

#include "obj.h"   /* OBJ_TAG_* / OBJ_CAP_* — one source of truth */

/* --- lifecycle ------------------------------------------------------ */

/* Allocate a byte-addressable object (ObjAlloc #0x100): `len` bytes, type
 * `tag`, initial caps `caps`. Returns the fresh capability, or a NULL
 * reference on firmware error (check objor_isnull). */
void *__or objor_alloc(int len, int tag, int caps);

/* Like objor_alloc but the storage is OR-typed (ObjAllocStore #0x106):
 * OREFLD/OREFST access it; integer OL/OS trap. `len` must be a multiple
 * of 8. Returns the capability, or a NULL reference on error. */
void *__or objor_alloc_store(int len, int tag, int caps);

/* Derive a SUB-reference of `src` carrying a subset of its caps
 * (ObjDerive #0x103): same gen/home/index, `caps` masked against src's.
 * Requires CAP_C on src. Returns the narrowed capability, or NULL on
 * error. The result shares src's object — objor_drop it (let it go out of
 * scope), never objor_free it. */
void *__or objor_derive(void *__or src, int caps);

/* Free the object `o` names and reclaim its storage (ObjFree #0x101).
 * `o` must carry CAP_V and be home-local. Returns firmware status
 * (0 = freed). Only call on a capability you OWN, never on a derived
 * sub-reference. */
int objor_free(void *__or o);

/* Release a derived/borrowed sub-reference. A capability VALUE owns no
 * table slot, so dropping one is just discarding the C value — this is a
 * no-op, provided so call sites can state intent (mirrors obj.h's
 * obj_drop, which had a handle slot to release). */
void objor_drop(void *__or o);

/* --- inspection (no memory access) ---------------------------------- */

int objor_isnull(void *__or o);          /* 1 if `o` is a null reference (OISN) */
int objor_eq(void *__or a, void *__or b);/* 1 if a,b name the same object (OEQ) */
int objor_len(void *__or o);             /* storage length in bytes (OLEN) */
int objor_tag(void *__or o);             /* type-tag word (OTAG) */
int objor_caps(void *__or o);            /* capability bits (OCAP) */

/* --- byte access at offset 0 (needs R / W caps) --------------------- */

int  objor_loadw(void *__or o);          /* OLW word at offset 0 (needs R) */
void objor_storew(void *__or o, int v);  /* OSW word at offset 0 (needs W) */

/* --- messaging ------------------------------------------------------ */

/* Attach a receive queue (`depth` slots) to `svc` so it can receive SENDs
 * (ReceiveQueueAttach #0x203). `svc` needs S|V. Returns firmware status
 * (0 = attached). */
int objor_queue_attach(void *__or svc, int depth);

/* SEND to `recip` (needs S) with no OR payload — just the four integer
 * words a0..a3 in R4..R7 (ObjSend). O2..O4 are nulled. Returns 0 (SEND
 * traps on error rather than returning status). */
int objor_send(void *__or recip, int a0, int a1, int a2, int a3);

/* SEND to `recip` carrying ONE capability payload `p2` (rides O2, the
 * reply-cap / subscribe-cap slot the recv side reads) plus the four
 * integer words a0..a3 in R4..R7. O3/O4 are nulled. Returns 0. */
int objor_send_cap(void *__or recip, void *__or p2,
                   int a0, int a1, int a2, int a3);

/* Block on `q`'s receive queue until a message arrives (ReceiveQueuePoll
 * #0x204, infinite timeout), then RETURN the capability the reply carries
 * in its O2 register (the resolved-ref / handed-over-cap convention) and
 * write the reply's R3 status word to *out_word (when non-NULL). Returns a
 * NULL reference if the poll fails or the reply carried no cap — check
 * objor_isnull on the result. */
void *__or objor_recv_cap(void *__or q, int *out_word);

/* --- inherited-capability adoption (O-register <-> value bridge) ----- */

/* The value API can MINT caps (objor_alloc/derive) and RECEIVE them
 * (objor_recv_cap), but a program's first caps arrive in object registers
 * the loader or a parent set up — boot services, or a cap a parent parks
 * for a child it spawns.  These two ops bridge such an O-register cap into
 * and out of the value world (the same single OMOV the handle API's
 * obj_adopt_oN / obj_park_oN use).  O7 is the conventional spawn-handoff
 * register: TaskCreate copies O1..O15 to the child, so a cap parked in O7
 * before a spawn is inherited by the child (see
 * examples/cc/multitask/concurrent.c). */

/* Park capability `o` into object-register O7 so a task spawned next
 * inherits it. */
void objor_stash_o7(void *__or o);

/* Adopt the capability parked/inherited in O7 (see objor_stash_o7) into a
 * `void *__or` value. */
void *__or objor_adopt_o7(void);

#endif /* OBJ_OR_H */

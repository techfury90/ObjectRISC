/*
 * orvec.h — a growable array of capability slots (object references), built
 * on the register-indexed OREFLD/OREFST primitive (objor_vget / objor_vset /
 * objor_vclear). This is the composition primitive for object trees: a
 * document Section holds its Blocks in an orvec, a widget container holds its
 * children, and so on — the same type wherever an object owns an ordered,
 * growable set of other objects.
 *
 * REPRESENTATION. An orvec IS its backing store — a single OR-typed object
 * (objor_alloc_store) of 8-byte reference slots — plus a LENGTH the caller
 * tracks in one int. Capacity is intrinsic: orvec_cap() derives it from the
 * object's size, so there is no separate capacity field and growth simply
 * yields a larger store. There is no hidden container object; the caller (or
 * the enclosing object, e.g. a Section) decides where the store cap and the
 * length live. Element access is the raw indexed ops on the store cap
 * (objor_vget / objor_vset / objor_vclear, declared in obj_or.h) — an orvec
 * value is a `void *__or`.
 *
 * OWNERSHIP.
 *   - An orvec OWNS its backing store: orvec_free releases it, and orvec_push
 *     releases the OLD store when it grows. Hold the store cap with CAP_V.
 *   - An orvec does NOT own its elements. A slot holds a borrowed reference;
 *     orvec_free does not touch the elements. Freeing or freezing an element
 *     is the caller's (or the lifecycle layer's) responsibility.
 *   - A NULL slot (objor_isnull(objor_vget(v, i))) means the position is empty
 *     or has been frozen away. A fresh store is all-null with length 0; a
 *     position in [0, len) may be live or null.
 *
 * FREEZE-ON-SCROLL-AWAY — the lifecycle policy this design is built to admit,
 * layered ABOVE orvec (not implemented here). A large collection cannot keep
 * every element live: 100 live blocks are 100 live firmware objects, spending
 * the descriptor budget REGARDLESS of how they are indexed. Register-indexed
 * access solved the ADDRESSING problem, not the LIFETIME one — they are
 * separate concerns. So the lifecycle layer keeps a viewport-scale window of
 * elements live and freezes the rest: it serialises a scrolled-away element's
 * bytes into a backing byte log (a future orbuf) with an offset index,
 * objor_free's the element, and objor_vclear's its slot; thawing reverses it.
 * orvec admits this by allowing null slots and never owning elements — the
 * serialisation and index are the orbuf layer's concern.
 */

#ifndef ORVEC_H
#define ORVEC_H

#include "liborisc.h"   /* task_t etc. — obj.h (pulled in via obj_or.h) uses
                         * them but does not include liborisc.h itself */
#include "obj_or.h"

/* Allocate a backing store for `cap` reference slots (all null), logical
 * length 0. `cap` < 1 is treated as 1. Returns the store capability
 * (R|W|V|C) or a null reference on allocation failure. */
void *__or orvec_new(int cap);

/* Place `elem` at index `len` (the caller's current length), growing the
 * store first if it is full (allocates a store of twice the capacity, copies
 * the live slots across, frees the old store). Returns the current store
 * capability — equal to `store` unless it grew, in which case the caller MUST
 * adopt the returned value in place of `store` — or a null reference on
 * allocation failure (the original `store` is left intact; do not overwrite
 * it with the null). The caller owns the length: increment it after a
 * successful push. `len` is passed by value deliberately — taking the address
 * of a caller local across an `__or`-frame miscompiles under pcc-orisc. */
void *__or orvec_push(void *__or store, void *__or elem, int len);

/* Capacity (number of slots) of `store`, derived from its size. */
int orvec_cap(void *__or store);

/* Release the backing store (NOT its elements — see OWNERSHIP above). */
void orvec_free(void *__or store);

#endif /* ORVEC_H */

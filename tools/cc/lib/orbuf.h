/*
 * orbuf.h — a growable BYTE buffer: the byte-collection sibling of orvec
 * (which holds capability slots). An orbuf holds raw bytes — a document Run's
 * text, a serialisation scratch, and the backing byte-log that
 * freeze-on-scroll-away serialises frozen elements into.
 *
 * REPRESENTATION. An orbuf IS its backing store — a single byte-typed ObjAlloc
 * object. Capacity is intrinsic (orbuf_cap = OLEN); the caller tracks the used
 * length. Bytes move in BULK via ObjFetchBytes, object-to-object: append
 * copies a span from a source object into the buffer, and read copies a span
 * back out into a destination object. Per-byte access at a runtime offset is
 * not offered (integer OL/OS ops are immediate-offset only); bulk
 * object-to-object copy is both what the ISA gives cheaply and what the
 * freeze / run-text uses actually want. An orbuf value is a `void *__or`.
 *
 * OWNERSHIP. An orbuf owns its backing store (append frees the old store on
 * growth; free releases it). It does not retain the source or destination
 * objects passed to append/read — those stay the caller's. The bytes are
 * COPIED, so a source object may be freed immediately after an append.
 *
 * FREEZE-ON-SCROLL-AWAY uses orbuf as the byte-log: to freeze an element the
 * lifecycle layer appends the element's bytes (orbuf_append(buf, len, elem, 0,
 * elem_len)), records the (offset, length) in an index, frees the element, and
 * nulls its orvec slot; to thaw, it allocates a fresh object and reads the span
 * back with orbuf_read. The index and the freeze/thaw protocol sit ABOVE orbuf
 * (see orvec.h) — orbuf is just the byte store.
 */

#ifndef ORBUF_H
#define ORBUF_H

#include "liborisc.h"   /* task_t etc. — obj.h (via obj_or.h) uses them */
#include "obj_or.h"

/* Allocate a byte buffer of `cap` bytes (cap < 1 is treated as 1), used length
 * 0. Returns the buffer capability (R|W|V|C) or a null reference on failure. */
void *__or orbuf_new(int cap);

/* Copy `n` bytes from `src[src_off]` to the buffer at index `len` (the
 * caller's current length), growing first (to max(2*cap, len+n)) if it would
 * overflow. Returns the buffer capability — equal to `buf` unless it grew, in
 * which case the caller MUST adopt the returned value — or a null reference on
 * allocation failure (the original `buf` is left intact). The caller owns the
 * length: add `n` after a successful append. `src` may be freed afterwards
 * (its bytes have been copied). */
void *__or orbuf_append(void *__or buf, int len, void *__or src,
                        int src_off, int n);

/* Copy `n` bytes from the buffer at `off` into `dst[dst_off]`. */
void orbuf_read(void *__or buf, int off, void *__or dst, int dst_off, int n);

/* Capacity (allocated size) of the buffer in bytes. */
int orbuf_cap(void *__or buf);

/* Release the backing store. */
void orbuf_free(void *__or buf);

#endif /* ORBUF_H */

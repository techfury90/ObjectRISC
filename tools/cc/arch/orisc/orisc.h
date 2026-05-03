/*
 * orisc.h — Object RISC architectural primitives accessible from C.
 *
 * These are macros over extended inline asm. They take or produce
 * `__or`-qualified pointers (held in OR-file slots, CLASSC) and
 * standard C ints/pointers (held in GPRs). Use them when you have
 * `register __or T *p __asm__("oN")` declarations and need to
 * inspect or operate on the OR slots.
 *
 * Macros use GCC statement-expression syntax, which pcc supports.
 *
 * For the longer term, these should become true compiler intrinsics
 * (`__builtin_orisc_*`) once the `__or` calling convention is in
 * place — at that point, regular C functions taking `__or` args
 * will work and we can replace the macros with real prototypes.
 */

#ifndef ORISC_H
#define ORISC_H

/* ---- Object-register inspection (no memory access) ---- */

/* OEQ — return 1 if two object references are equal (same gen,
 * home, index; capability bits are NOT compared). */
#define oref_eq(a, b) ({ \
	int _r; \
	asm("oeq %0, %1, %2" : "=r"(_r) : "r"(a), "r"(b)); \
	_r; \
})

/* OISN — return 1 if the reference is null. */
#define oref_isnull(a) ({ \
	int _r; \
	asm("oisn %0, %1" : "=r"(_r) : "r"(a)); \
	_r; \
})

/* OLEN — return the length (in bytes) of the referenced object's
 * storage. Traps stale-reference if the descriptor's generation
 * doesn't match. */
#define oref_len(a) ({ \
	int _r; \
	asm("olen %0, %1" : "=r"(_r) : "r"(a)); \
	_r; \
})

/* OTAG — return the type-tag word of the referenced object. */
#define oref_tag(a) ({ \
	int _r; \
	asm("otag %0, %1" : "=r"(_r) : "r"(a)); \
	_r; \
})

/* OHOME — return the home processor ID of the reference. */
#define oref_home(a) ({ \
	int _r; \
	asm("ohome %0, %1" : "=r"(_r) : "r"(a)); \
	_r; \
})

/* OCAP — return the capability bits of the reference. */
#define oref_caps(a) ({ \
	int _r; \
	asm("ocap %0, %1" : "=r"(_r) : "r"(a)); \
	_r; \
})

/* ---- Typed loads/stores through an OR pointer ---- */

/* OL/OS at offset 0 — narrow byte/half/word loads and stores.
 * The reference must carry the appropriate cap (R for loads,
 * W for stores) and not be null/stale; mismatch traps. */

#define oref_loadb(a) ({ \
	int _r; \
	asm("olb %0, 0(%1)" : "=r"(_r) : "r"(a)); \
	_r; \
})
#define oref_loadbu(a) ({ \
	int _r; \
	asm("olbu %0, 0(%1)" : "=r"(_r) : "r"(a)); \
	_r; \
})
#define oref_loadh(a) ({ \
	int _r; \
	asm("olh %0, 0(%1)" : "=r"(_r) : "r"(a)); \
	_r; \
})
#define oref_loadhu(a) ({ \
	int _r; \
	asm("olhu %0, 0(%1)" : "=r"(_r) : "r"(a)); \
	_r; \
})
#define oref_loadw(a) ({ \
	int _r; \
	asm("olw %0, 0(%1)" : "=r"(_r) : "r"(a)); \
	_r; \
})

#define oref_storeb(a, v) \
	asm("osb %1, 0(%0)" : : "r"(a), "r"(v))
#define oref_storeh(a, v) \
	asm("osh %1, 0(%0)" : : "r"(a), "r"(v))
#define oref_storew(a, v) \
	asm("osw %1, 0(%0)" : : "r"(a), "r"(v))

/* ---- Memory ordering ---- */

/* OFENCE — barrier between OR-mediated and mapped-page accesses
 * to the same object. */
#define oref_fence() asm("ofence")

#endif /* ORISC_H */

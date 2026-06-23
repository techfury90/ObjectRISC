/*
 * Object RISC backend for pcc.
 *
 * Initial sketch — establishes the type sizes, register files, ABI
 * constants, and class/overlap tables enough to start writing
 * code.c / local.c / local2.c / table.c against. Modeled on the
 * top-level pcc 32-bit MIPS port (Enoksson/Olsson 2005), which is
 * the closest existing template: same load-store RISC shape, same
 * R/I/J formats, same HI/LO pair for MULT/DIV, same branch-delay
 * semantics. Distinguishing Object RISC bits:
 *
 *   - Big-endian (Volume I).
 *   - Sixteen 64-bit Object Registers (O0..O15) form a *separate*
 *     register file. References cannot be smuggled through integer
 *     storage; the architecture enforces this statically. ORs are
 *     exposed to C as a third register class (CLASSC) holding
 *     `__or`-qualified pointer-shaped values.
 *   - O0 is hardwired null, O1..O4 are object arguments / first OR
 *     return. Volume VII §2.1 specifies O9..O12 as callee-preserved
 *     and O5..O8 / O13..O15 as caller-saved, but this backend
 *     currently treats *all* of O1..O15 as caller-saved — see the
 *     RSTATUS comment below for the spill-path reason.
 *   - Four integer arguments (R4..R7), not eight; matches MIPS O32.
 *   - No floating-point unit yet (Volume I deferred); CLASSC slots
 *     formerly used for FP regs in the MIPS port are repurposed
 *     here for the OR file.
 *
 * Reference: Volume II §3 (register usage), Volume VII §2 (ABI).
 */

/*
 * Convert (multi-)character constant to integer. Same approach as MIPS
 * (left-justified within an int).
 */
#define makecc(val,i)	lastcon = (lastcon<<8)|((val<<24)>>24);

#define ARGINIT		(16*8)	/* # bits above fp where arguments start */
#define AUTOINIT	(0)	/* # bits below fp where automatics start */

/*
 * Storage space requirements (bits). Matches Volume II §3 / Volume VII §2.
 */
#define SZCHAR		8
#define SZBOOL		32
#define SZINT		32
#define SZFLOAT		32
#define SZDOUBLE	64
#define SZLDOUBLE	64
#define SZLONG		32
#define SZSHORT		16
#define SZLONGLONG	64
#define SZPOINT(t)	32

/*
 * Alignment constraints (bits).
 */
#define ALCHAR		8
#define ALBOOL		32
#define ALINT		32
#define ALFLOAT		32
#define ALDOUBLE	64
#define ALLDOUBLE	64
#define ALLONG		32
#define ALLONGLONG	64
#define ALSHORT		16
#define ALPOINT		32
#define ALSTRUCT	64
#define ALSTACK		32

/*
 * Min/max integer constants.
 */
#define	MIN_CHAR	-128
#define	MAX_CHAR	127
#define	MAX_UCHAR	255
#define	MIN_SHORT	-32768
#define	MAX_SHORT	32767
#define	MAX_USHORT	65535
#define	MIN_INT		(-0x7fffffff-1)
#define	MAX_INT		0x7fffffff
#define	MAX_UNSIGNED	0xffffffffU
#define	MIN_LONG	MIN_INT
#define	MAX_LONG	0x7fffffffL
#define	MAX_ULONG	0xffffffffUL
#define	MIN_LONGLONG	(-0x7fffffffffffffffLL-1)
#define	MAX_LONGLONG	0x7fffffffffffffffLL
#define	MAX_ULONGLONG	0xffffffffffffffffULL

#undef	CHAR_UNSIGNED
#define BOOL_TYPE	INT

typedef	long long CONSZ;
typedef	unsigned long long U_CONSZ;
typedef long long OFFSZ;

#define CONFMT	"%lld"		/* format for printing constants */
#define LABFMT	"L%d"		/* format for printing labels */
#define	STABLBL	"LL%d"		/* format for stab (debugging) labels */

#define BACKAUTO 		/* stack grows negatively for automatics */
#define BACKTEMP 		/* stack grows negatively for temporaries */

#undef	FIELDOPS		/* no bit-field instructions */
#define TARGET_ENDIAN TARGET_BE	/* Object RISC is big-endian (Vol I) */
#define	MYALIGN

/* Definitions mostly used in pass 2 */

#define BYTEOFF(x)	((x)&03)

#define	szty(t)		(((t) == DOUBLE || (t) == LDOUBLE || \
	DEUNSIGN(t) == LONGLONG) ? 2 : 1)

/*
 * Register names — must match rnames[] and rstatus[] in local2.c.
 *
 * Physical register encoding:
 *   0..31    GPRs R0..R31  (CLASSA)
 *   32..62   GPR adjacent pairs (CLASSB) — (R2:R3, R4:R5, ..., R30:R31)
 *            for 64-bit longlong values living in two adjacent GPRs.
 *   63..78   ORs O0..O15  (CLASSC)
 *
 * The pair register numbers chosen below mirror the MIPS port's
 * approach: every adjacent (Rk, Rk+1) pair gets one synthesized
 * register name, and ROVERLAP teaches the allocator that the pair
 * conflicts with both halves.
 */

/* CLASSA: 32 GPRs, named per Volume II §3 */
#define	R0	0	/* hardwired zero */
#define	R1	1	/* assembler temporary */
#define	R2	2	/* return value lo */
#define	R3	3	/* return value hi (and longlong return hi) */
#define	R4	4	/* arg 0 */
#define	R5	5	/* arg 1 */
#define	R6	6	/* arg 2 */
#define	R7	7	/* arg 3 / PROCID at boot */
#define	R8	8	/* caller-saved */
#define	R9	9
#define	R10	10
#define	R11	11
#define	R12	12
#define	R13	13
#define	R14	14
#define	R15	15
#define	R16	16	/* callee-saved */
#define	R17	17
#define	R18	18
#define	R19	19
#define	R20	20
#define	R21	21
#define	R22	22
#define	R23	23
#define	R24	24	/* caller-saved */
#define	R25	25
#define	R26	26
#define	R27	27
#define	R28	28
#define	SP	29	/* R29 — stack pointer */
#define	FP	30	/* R30 — frame pointer */
#define	RA	31	/* R31 — link register */

/* CLASSB: adjacent-GPR pairs for 64-bit longlong. One pair per even
 * starting register (R0:R1 .. R30:R31). We keep all 16 pair slots so
 * the allocator can always find a fresh pair. */
#define	R0R1	32
#define	R2R3	33
#define	R4R5	34
#define	R6R7	35
#define	R8R9	36
#define	R10R11	37
#define	R12R13	38
#define	R14R15	39
#define	R16R17	40
#define	R18R19	41
#define	R20R21	42
#define	R22R23	43
#define	R24R25	44
#define	R26R27	45
#define	R28R29	46	/* clobbers SP — never allocated, present for index */
#define	R30R31	47	/* clobbers FP/RA — same */

/* CLASSC: 16 Object Registers per Volume II §3.2 */
#define	O0	48	/* hardwired null */
#define	O1	49	/* OR arg 0 / first OR return */
#define	O2	50	/* OR arg 1 */
#define	O3	51	/* OR arg 2 */
#define	O4	52	/* OR arg 3 */
#define	O5	53	/* caller-saved */
#define	O6	54
#define	O7	55
#define	O8	56
#define	O9	57	/* callee-preserved */
#define	O10	58
#define	O11	59
#define	O12	60
#define	O13	61	/* caller-saved */
#define	O14	62
#define	O15	63

#define	MAXREGS		64
#define	NUMCLASS	3

#define	RETREG(x)	(ISOREFT(x) ? O1 : \
			 DEUNSIGN(x) == LONGLONG ? R2R3 : R2)
				/* Object RISC: capabilities return in O1 */
#define	FPREG		FP	/* frame pointer */

/*
 * RSTATUS — initial allocator state for each physical register. One
 * entry per register in numerical order; same length as MAXREGS.
 *
 *   SAREG | TEMPREG  : caller-saved general-purpose
 *   SAREG | PERMREG  : callee-preserved (no caller spill needed)
 *   SBREG | ...      : same flags but for class-B (GPR pairs)
 *   SCREG | ...      : same for class-C (ORs)
 *   0                : reserved — not allocatable (R0 zero, SP, etc.)
 */
#define RSTATUS \
	0,				/* R0 — hardwired zero */	\
	0,				/* R1 — at: reserved for the */	\
					/* assembler's lw/sw-with-label */\
					/* synthetic and other pseudo-ops*/\
					/* that need a clobberable temp */\
	SAREG|TEMPREG, SAREG|TEMPREG,	/* R2..R3 — return values */	\
	SAREG|TEMPREG, SAREG|TEMPREG,	/* R4..R5 — args */		\
	SAREG|TEMPREG, SAREG|TEMPREG,	/* R6..R7 — args (R7 = PROCID) */ \
	SAREG|TEMPREG, SAREG|TEMPREG,	/* R8..R9 — caller-saved */	\
	SAREG|TEMPREG, SAREG|TEMPREG,					\
	SAREG|TEMPREG, SAREG|TEMPREG,					\
	SAREG|TEMPREG, SAREG|TEMPREG,					\
	0, 0, 0, 0,			/* R16..R23 — callee-preserved */ \
	0, 0, 0, 0,			/* (marked unallocatable for now */ \
					/*  to avoid pcc's allocator emitting */ \
					/*  spurious save/restore code; cuts */ \
					/*  ~16 instructions per function with */ \
					/*  no functional cost on small demos. */ \
					/*  Re-enable with PERMREG once pcc */ \
					/*  optimizer eliminates dead saves.) */ \
	SAREG|TEMPREG, SAREG|TEMPREG,	/* R24..R28 — caller-saved */	\
	SAREG|TEMPREG, SAREG|TEMPREG, SAREG|TEMPREG,			\
	0, 0, 0,			/* SP, FP, RA — reserved */	\
									\
	/* CLASSB pairs — only the pairs that don't overlap reserved	\
	 * regs are allocatable. R0R1 contains R0 (zero), R28R29 hits	\
	 * SP, R30R31 hits FP+RA, so those three are unallocatable. */	\
	0,				/* R0R1 (hits R0) */		\
	SBREG|TEMPREG, SBREG|TEMPREG,	/* R2R3, R4R5 */		\
	SBREG|TEMPREG, SBREG|TEMPREG,	/* R6R7, R8R9 */		\
	SBREG|TEMPREG, SBREG|TEMPREG,	/* R10R11, R12R13 */		\
	SBREG|TEMPREG,			/* R14R15 */			\
	0, 0,				/* R16R17, R18R19 */		\
	0, 0,				/* R20R21, R22R23 */		\
	SBREG|TEMPREG, SBREG|TEMPREG,	/* R24R25, R26R27 */		\
	0, 0,				/* R28R29, R30R31 — reserved */	\
									\
	/* CLASSC: O0 hardwired null. ONLY O1..O4 are allocatable by the
	 * compiler (the OR arg/return scratch set). O5..O15 are reserved
	 * (marked 0 / non-allocatable) because the libc runtime parks
	 * long-lived global capabilities there and reaches them via inline
	 * asm that the compiler sees only as opaque clobbers:
	 *   O5..O7 surfaces (console/keyboard/grid), O8 dir/parent,
	 *   O9 term mailbox, O10 pointer mailbox, O11 boot stack,
	 *   O12 the TASK TABLE (objstore), O13..O15 boot code/self/data.
	 * If the allocator were allowed to colour any of these (it can
	 * under CLASSC register pressure — e.g. the OBJSTORE-memory-
	 * variable access sequence loads capabilities into CLASSC scratch)
	 * it would silently corrupt libc state; allocating O12 would
	 * destroy the entire task table. Reserving them confines the
	 * compiler to O1..O4 and makes O12 safe to use as the fixed
	 * OBJSTORE anchor base. The ≤3-`__or`-arg/param v1 limit keeps
	 * peak CLASSC pressure at 4, so four scratch ORs suffice.
	 *
	 * Volume VII §2.4's "O9..O12 callee-preserved" convention is
	 * superseded here: per-frame OBJSTORE homing (anchored in an O12
	 * slot, chained for recursion) is how `__or` values survive calls,
	 * so no O-register needs to be callee-preserved. */ \
	0,				/* O0 — hardwired null */	\
	SCREG|TEMPREG, SCREG|TEMPREG,	/* O1..O2 — args/returns/scratch */ \
	SCREG|TEMPREG, SCREG|TEMPREG,	/* O3..O4 — args/returns/scratch */ \
	0, 0,				/* O5..O6 — reserved (libc global) */ \
	0, 0,				/* O7..O8 — reserved (libc global) */ \
	0, 0,				/* O9..O10 — reserved (libc global) */ \
	0, 0,				/* O11..O12 — reserved (O12=task tbl)*/ \
	0, 0, 0,			/* O13..O15 — reserved (libc global) */

/*
 * ROVERLAP — for each physical register, the list of other physical
 * registers that conflict with it (terminated by -1). Each GPR is
 * disjoint from every other GPR (so the list is just `-1`), but each
 * pair conflicts with both of its halves.
 *
 * Length must be MAXREGS rows.
 */
#define ROVERLAP \
	{ -1 },				/* R0 */				\
	{ -1 },				/* R1 */				\
	{ R2R3, -1 },			/* R2 */				\
	{ R2R3, -1 },			/* R3 */				\
	{ R4R5, -1 },			/* R4 */				\
	{ R4R5, -1 },			/* R5 */				\
	{ R6R7, -1 },			/* R6 */				\
	{ R6R7, -1 },			/* R7 */				\
	{ R8R9, -1 },			/* R8 */				\
	{ R8R9, -1 },			/* R9 */				\
	{ R10R11, -1 },			/* R10 */				\
	{ R10R11, -1 },			/* R11 */				\
	{ R12R13, -1 },			/* R12 */				\
	{ R12R13, -1 },			/* R13 */				\
	{ R14R15, -1 },			/* R14 */				\
	{ R14R15, -1 },			/* R15 */				\
	{ R16R17, -1 },			/* R16 */				\
	{ R16R17, -1 },			/* R17 */				\
	{ R18R19, -1 },			/* R18 */				\
	{ R18R19, -1 },			/* R19 */				\
	{ R20R21, -1 },			/* R20 */				\
	{ R20R21, -1 },			/* R21 */				\
	{ R22R23, -1 },			/* R22 */				\
	{ R22R23, -1 },			/* R23 */				\
	{ R24R25, -1 },			/* R24 */				\
	{ R24R25, -1 },			/* R25 */				\
	{ R26R27, -1 },			/* R26 */				\
	{ R26R27, -1 },			/* R27 */				\
	{ -1 },				/* R28 (no allocatable pair) */		\
	{ -1 },				/* SP */				\
	{ -1 },				/* FP */				\
	{ -1 },				/* RA */				\
									\
	/* Pairs */							\
	{ R0, -1 },			/* R0R1 */				\
	{ R2, R3, -1 },			/* R2R3 */				\
	{ R4, R5, -1 },			/* R4R5 */				\
	{ R6, R7, -1 },			/* R6R7 */				\
	{ R8, R9, -1 },			/* R8R9 */				\
	{ R10, R11, -1 },		/* R10R11 */				\
	{ R12, R13, -1 },		/* R12R13 */				\
	{ R14, R15, -1 },		/* R14R15 */				\
	{ R16, R17, -1 },		/* R16R17 */				\
	{ R18, R19, -1 },		/* R18R19 */				\
	{ R20, R21, -1 },		/* R20R21 */				\
	{ R22, R23, -1 },		/* R22R23 */				\
	{ R24, R25, -1 },		/* R24R25 */				\
	{ R26, R27, -1 },		/* R26R27 */				\
	{ R28, SP, -1 },		/* R28R29 (reserved) */			\
	{ FP, RA, -1 },			/* R30R31 (reserved) */			\
									\
	/* Object registers — disjoint from each other and from GPRs. */	\
	{ -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 },	\
	{ -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 },

/*
 * Per-frame OBJSTORE spill for `__or` autos/params that must survive a
 * call. ORs are caller-saved and cannot be stored to byte memory (the
 * capability invariant), so an `__or` local/param lives in a slot of a
 * per-frame OR-typed OBJSTORE (allocated in the prologue, freed in the
 * epilogue) and is loaded into an O-register transiently per use.
 *
 * The spill objstore's own ref must itself survive the calls it
 * protects against, and there is no free callee-preserved O-register to
 * hold it — so it is anchored in a dedicated slot of the always-live
 * O12 task table, and frames are chained through the spill object's
 * slot 0 (parent ref) for recursion safety. ORSPILL_ANCHOR is the byte
 * offset of that anchor slot within the O12 task table; it MUST match
 * the reserved slot in tools/cc/lib/task.c. Slot 0 of each per-frame
 * objstore is the chain link; `__or` homes start at byte offset 8.
 */
#define ORSPILL_ANCHOR	1696	/* O12 byte offset; matches task.c */
#define ORSPILL_TAG	0x4102	/* TAG_DATA */
#define ORSPILL_CAPS	0x03	/* CAP_R | CAP_W */
#define ORSPILL_BASE	8	/* first __or home (slot 0 = chain link) */

#define GCLASS(x)	((x) < 32 ? CLASSA : (x) < 48 ? CLASSB : CLASSC)
/*
 * PCLASS: the register class a node should be allocated to. Object-
 * reference pointers (OBIT set in the type word, per mip/manifest.h)
 * live in the OR file (CLASSC) — Volume III's architectural
 * commitment. Because OBIT rides n_type, gclass() already routes them
 * to CLASSC, so the gclass fallback below covers the normal case
 * without consulting the (creator-zeroed) qualifier word.
 *
 * The REG-range arm is kept as a belt-and-suspenders for nodes bound
 * to an OR slot via `register __or T *p __asm__("oN")`, whose physical
 * reg number lands in CLASSC's range.
 */
#define PCLASS(p)	(((p)->n_op == REG && (p)->n_rval >= 48 \
			       && (p)->n_rval < 64) \
			    ? (1 << CLASSC) \
			    : (1 << gclass((p)->n_type)))
#define DECRA(x,y)	(((x) >> (y*6)) & 63)   /* decode encoded regs */
#define ENCRA(x,y)	((x) << (6+y*6))        /* encode regs in int */
#define ENCRD(x)	(x)			/* encode dest reg in n_reg */

int COLORMAP(int c, int *r);

#define SPCON           (MAXSPECIAL+1)  /* positive constant */

/*
 * TARGET_BUILTINS — Object RISC needs the standard varargs builtins
 * and (eventually) intrinsic builtins for the architecture's novel
 * primitives:
 *
 *   __builtin_orisc_send(target, payload...)
 *   __builtin_orisc_oref_load(obj, offset)
 *   __builtin_orisc_oref_store(obj, offset, ref)
 *   __builtin_orisc_ofence()
 *   __builtin_orisc_call(prim_num, args...)   for raw firmware CALL
 *
 * These are stubbed out here for now — the actual lowering happens in
 * code.c / table.c. The list below is the standard varargs set,
 * adapted from the MIPS port's TARGET_BUILTINS but renamed.
 */
#define TARGET_STDARGS
#define TARGET_BUILTINS							\
	{ "__builtin_stdarg_start", orisc_builtin_stdarg_start,		\
						0, 2, 0, VOID },	\
	{ "__builtin_va_start", orisc_builtin_stdarg_start,		\
						0, 2, 0, VOID },	\
	{ "__builtin_va_arg", orisc_builtin_va_arg, BTNORVAL|BTNOPROTO,	\
							2, 0, 0 },	\
	{ "__builtin_va_end", orisc_builtin_va_end, 0, 1, 0, VOID },	\
	{ "__builtin_va_copy", orisc_builtin_va_copy, 0, 2, 0, VOID },

#ifdef LANG_CXX
#define P1ND struct node
#else
#define P1ND struct p1node
#endif
struct node;
struct bitable;
P1ND *orisc_builtin_stdarg_start(const struct bitable *, P1ND *a);
P1ND *orisc_builtin_va_arg(const struct bitable *, P1ND *a);
P1ND *orisc_builtin_va_end(const struct bitable *, P1ND *a);
P1ND *orisc_builtin_va_copy(const struct bitable *, P1ND *a);
#undef P1ND

/*
 * No floating-point unit yet (Volume I leaves FP for a future
 * revision). The IEEE definitions below pretend floats are 32-bit
 * IEEE binary32 / 64-bit binary64 to keep the front end happy with
 * float / double declarations; actual codegen for FP operations will
 * have to soft-float through the runtime until silicon FP arrives.
 */
#define USE_IEEEFP_32
#define FLT_PREFIX	IEEEFP_32
#define USE_IEEEFP_64
#define DBL_PREFIX	IEEEFP_64
#define LDBL_PREFIX	IEEEFP_64
#define DEFAULT_FPI_DEFS { &fpi_binary32, &fpi_binary64, &fpi_binary64 }
